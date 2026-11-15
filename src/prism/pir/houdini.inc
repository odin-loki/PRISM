// Loop invariants for PIR by Houdini over loop cuts (included by encode.cpp,
// inside its anonymous namespace; docs/PIR.md "Loop invariants").
//
// The function is encoded in cut mode (Encoding::cut_mode, unwind 1): at the
// header instance of every loop the entry state is recorded, then the
// header phis and the loop's write footprint are havocked (an arbitrary
// later header visit) and that state is recorded; every back edge is an
// unwinding cut and records the next header state. Candidate invariants
// come from templates over the header phis, values defined before the loop
// (scope values), constants and the objects the loop accesses. Houdini
// keeps the candidates that are
//
//   base      true in the entry state, and
//   step      true in the next state of every back edge,
//
// assuming, at every header, the surviving candidates of that loop (and of
// the loops encoded before the point being checked) in its havocked state.
// Only the fixpoint is used: the function is PROVED-UNBOUNDED when, with
// every surviving candidate assumed at every havocked header, no property
// instance of the cut program is violated. Candidates that fail a check, or
// whose check has no answer, are dropped; nothing unproved is ever assumed.
//
// Why this is sound. Positions are encoding (topological) order; on the one
// path a model makes reachable they are execution order. Let a run of the
// function reach its first violation V (or none). By induction over the
// header visits before V, every surviving candidate holds at each of them:
// the abstract path from the function entry to the visit follows the run,
// with every earlier loop's havocked state chosen as the run's state at
// that loop's last header visit (true for its candidates by the induction
// hypothesis; the loop allocates and frees nothing, and every byte it
// writes is in its footprint, so memory outside the footprint is the
// prefix's). A first visit is a base query, a later one a step query from
// the previous visit; both queries assume only the havoc assumptions of
// loops whose header comes earlier on the path and that no property before
// the point is violated (true before V). Then the same abstract path reaches
// V, so the property query of V is satisfiable. Havoc assumptions are
// guarded by "no violation before this header", so a path that has already
// violated a property is never cut off by them (a violating path continues
// freely, as the bounded encoding does).
//
// Memory templates are universally quantified over an index x: "for every
// x < i: s[x] != 0". They are checked pointwise at one fresh constant x0
// (base and step valid for every x0 => the universal statement is
// inductive) and assumed at x0 and at the index terms of the encoding
// (instances of a true universal statement). Their loads read the recorded
// memory state exactly (SymMem::load_exact: no provenance shortcut), so a
// template means the bytes of that state for every x.

// PRISM_HOUDINI_TRACE=1: one stderr line per Houdini goal query (diagnostics only)
static bool houdini_trace() {
    static const bool on = [] {
        const char* e = prism::settings::cstr(prism::settings::houdini_trace());
        return e && *e && *e != '0';
    }();
    return on;
}

struct InvTerm {
    enum Kind { Phi, Scope, Const, Entry, Size } kind = Const;
    int idx = -1;  // Phi/Entry: phi index in the header; Scope/Size: variable
    uint64_t c = 0;
    unsigned w = 0;
};

struct InvCand {
    enum Kind { Cmp, Mask, Lin, MaxLe, Scale, ScaleV, Tri, DiffCmp, ScaleVK, ModLin, PtrIn, PtrAt, MemNZ, MemEq, MemNeC,
                MemInit, MemConst, MemFrame, MemScopeAt, MemLast, MemRingInit, MemScale } kind = Cmp;
    int loop = -1;
    Op rel = Op::Eq;       // Cmp
    InvTerm a, b;          // Cmp: a rel b; Mask: (a & mask) == (b & mask); Lin: a -/+ b == ea -/+ eb
    bool sum = false;      // Lin: a + b (else a - b). MemScopeAt: x <= index (else x < index)
    // Arithmetic templates, evaluated over mathematical integers (both sides
    // extended to 2w+4 bits, so nothing wraps; sext: the values are signed):
    //   MaxLe:  a <= max(entry(a), b)            (b a scope value; rel Sle/Ule)
    //   Scale:  a - entry(a) rel mask * (b - entry(b))      (rel Eq/Sle/Sge)
    //   ScaleV: a - entry(a) == v * (b - entry(b))           (v a scope value)
    //   Tri:    2 * (a - entry(a)) == b*(b+d) - entry(b)*(entry(b)+d), d = mask - 1
    //   ScaleVK: a - entry(a) rel mask * v * (b - entry(b))   (3w+4 bits; rel Eq/Sle/Sge)
    // Idiom templates (src/prism/pir/idioms/), in the phis' width:
    //   DiffCmp: (a - b) rel v           (wrapping, as the program computes it)
    //   ModLin:  ((a - b - v) & mask) == ((entry(a) - entry(b) - entry(v)) & mask)   (a, b, v phis)
    // Like every template they are only admitted through Houdini's base and
    // step checks; none is assumed.
    uint64_t mask = 0;
    // memory: for every x with lo <= x < ext(phi idx): P(x)
    int ip = -1;           // index phi
    bool sext = false;
    bool lo_entry = false; // lo = entry value of the index phi (else 0)
    int p = -1, q = -1;    // pointers (scope variables)
    int off = -1;          // MemEq/MemInit/MemScopeAt: scope offset into p (i64); -1 = none
    int rp = -1;           // PtrIn/PtrAt/MemLast: a pointer phi r (the last match so far)
    unsigned eb = 1;       // element bytes
    // MemRingInit: forall x < mask with (x - phi ip) mod mask < phi ip2: p[x] initialised
    // (a ring's occupied slots; ip = tail, ip2 = count, zero-extended)
    // MemScale: forall x < mask: p[x] rel v * (phi ip2 - entry(phi ip2)) over integers
    // (element and phi extended to 64 + max(widths) + 4 bits, signed when sext; v a
    // signed 64-bit constant; rel Sle/Sge). ip: the slot index phi (an index term only)
    int ip2 = -1;
    InvTerm v;             // MemNeC: element != trunc(v); MemConst: element == v (const)
    bool alive = true;
    bool ghost() const { return kind >= MemNZ; }
};

struct HoudiniOut {
    bool attempted = false;
    bool proved = false;
    std::string why;
    int rounds = 0;
    std::size_t candidates = 0;
    std::vector<std::vector<std::string>> invariants;  // per loop, PIR text of the survivors
    std::vector<std::vector<std::size_t>> kept;  // per loop, indices into cands
    std::vector<InvCand> cands;
    std::string footprint;
    int prechecks = 0;      // optimistic final queries (all candidates alive assumed)
    std::string cache;      // "", "hit (proof re-checked)", "hit (no proof)", "stored"
    double final_s = -1;    // seconds given to the final query at the fixpoint (-1: not asked)
    bool undecided = false; // some answer came from the time limit or an unknown
    std::size_t concrete_dropped = 0;  // candidates a concrete run falsified (dropped before any query)
    int concrete_runs = 0;
    std::string idiom;      // idioms matched and what their round did ("" : none matched)
};

// Where Houdini's outcome is cached (per function text, docs/PIR.md "Loop
// invariants"): dir empty = no cache.
struct HoudiniEnv {
    std::string cache_dir;
};

// Pointer-valued variables (allocation results, checked pointer arithmetic,
// copies, selects and phis with such an incoming value). A heuristic for
// choosing templates; no proof relies on it.
std::vector<char> pointer_vars(const Function& fn) {
    std::vector<char> ptr(fn.vars.size(), 0);
    auto is = [&](const Arg& a) { return !a.is_const && ptr[static_cast<std::size_t>(a.var)]; };
    bool changed = true;
    while (changed) {
        changed = false;
        auto set = [&](int d) {
            if (d >= 0 && !ptr[static_cast<std::size_t>(d)]) {
                ptr[static_cast<std::size_t>(d)] = 1;
                changed = true;
            }
        };
        for (auto& b : fn.blocks) {
            for (auto& p : b.phis) {
                // any pointer-valued incoming value (candidate generation only)
                for (auto& [pb, a] : p.in) {
                    (void)pb;
                    if (!a.is_const && is(a)) set(p.dst);
                }
            }
            for (auto& s : b.stmts) {
                if (s.kind == Stmt::Alloc) set(s.dst);
                if (s.kind != Stmt::Assign) continue;
                if (s.ptr_arith) set(s.dst);
                if (s.op == Op::Copy && !s.args.empty() && is(s.args[0])) set(s.dst);
                if (s.op == Op::Select && s.args.size() == 3 && is(s.args[1]) && is(s.args[2])) set(s.dst);
            }
        }
    }
    return ptr;
}

class Houdini {
public:
    // timeout_s: the solver time per query. With a function budget (hard_s
    // >= 0) the whole search gets what is left of it (search_s, the run's
    // Houdini budget, caps it further); without one, 6 times timeout_s
    // (search_s < 6 * timeout_s when the run's budget has less left).
    // hard_s >= 0: a hard cap (what is left of the function budget) that no
    // query passes, the final query and the per-query minimums included; the
    // run's Houdini budget (search_s alone) leaves the final query its own
    // minimum.
    Houdini(const Function& fn, const CfgInfo& g, const EncodeOptions& eo, double timeout_s, double search_s = -1,
            HoudiniEnv env = {}, double hard_s = -1)
        : fn_(fn), g_(g), eo_(eo), per_query_(timeout_s),
          deadline_(search_s >= 0 ? std::min(search_s, 6 * timeout_s) : 6 * timeout_s), hard_s_(hard_s),
          env_(std::move(env)) {
        if (hard_s_ >= 0) {
            deadline_ = search_s >= 0 ? std::min(search_s, hard_s_) : hard_s_;
            // a share of the hard cap is kept for the final query so the
            // search cannot starve it ($PRISM_HOUDINI_FINAL_RESERVE, a
            // fraction in [0, 0.9]; default 0.3)
            deadline_ = std::min(deadline_, (1.0 - final_reserve()) * hard_s_);
        }
    }
    static double final_reserve() {
        double r = 0.3;
        if (const char* v = std::getenv("PRISM_HOUDINI_FINAL_RESERVE")) {
            char* end = nullptr;
            const double x = std::strtod(v, &end);
            if (end != v && *end == 0 && x >= 0 && x <= 0.9) r = x;
        }
        return r;
    }

    HoudiniOut run();

private:
    // Loop idioms (src/prism/pir/idioms/): their instantiated templates are
    // searched first and alone (idiom_round_); without a proof the generic
    // family is searched with them added. Both rounds check everything.
    HoudiniOut run_search();
    bool idiom_round_ = false;
    std::vector<InvCand> idiom_cands_;
    std::map<std::string, std::string> idiom_by_text_;  // candidate text -> idiom
    void concrete_filter(HoudiniOut& out);
    const Function& fn_;
    const CfgInfo& g_;
    EncodeOptions eo_;
    double per_query_;
    double deadline_;
    double hard_s_;
    mutable bool mem_out_ = false;  // left() saw the soft memory cap
    HoudiniEnv env_;
    std::string cache_file_;   // "" : no cache
    bool from_cache_ = false;  // candidates restricted to a cached proof's survivors
    std::vector<std::string> cands_for_key_;
    int query_ = 0;
    prism::clock::Stopwatch t0_;
    std::vector<char> ptr_;
    std::vector<const Stmt*> def_;
    std::vector<char> signed_use_;

    // Over the encoder's soft memory cap (encode.cpp z3_over_soft_cap) the
    // search ends as if out of time: every unanswered goal is dropped, and no
    // query can push Z3 past its hard limit, after which Z3 is unusable for
    // every later function of the process (Law 7: one UNKNOWN, not many).
    double left() const {
        if (z3_over_soft_cap()) {
            mem_out_ = true;
            return 0;
        }
        return deadline_ - t0_.elapsed_s();
    }
    // what is left of the hard cap (no cap: effectively unbounded)
    double hard_left() const {
        if (hard_s_ < 0) return 1e18;
        return hard_s_ - t0_.elapsed_s();
    }
    // a query's Z3 timeout in ms: t seconds, never past the hard cap (1 ms at least)
    unsigned query_ms(double t) const { return static_cast<unsigned>(std::max(0.001, std::min(t, hard_left())) * 1000); }
    bool in_loop(int L, int b) const {
        return b >= 0 && g_.loops[static_cast<std::size_t>(L)].body[static_cast<std::size_t>(b)];
    }
    bool is_param(int v) const { return std::find(fn_.params.begin(), fn_.params.end(), v) != fn_.params.end(); }
    // v is defined before the loop and its value can be read at the header
    bool scope_var(int L, int v) const {
        if (v < 0) return false;
        if (is_param(v)) return true;
        int d = g_.def_block[static_cast<std::size_t>(v)];
        if (d < 0 || in_loop(L, d)) return false;
        int H = g_.loops[static_cast<std::size_t>(L)].header;
        if (!g_.dom[static_cast<std::size_t>(H)][static_cast<std::size_t>(d)]) return false;
        const auto& Ld = g_.loops_of[static_cast<std::size_t>(d)];
        const auto& Lh = g_.loops_of[static_cast<std::size_t>(H)];
        return Ld.size() <= Lh.size() && std::equal(Ld.begin(), Ld.end(), Lh.begin());
    }
    int base_of(int L, int v) const {
        for (int guard = 0; v >= 0 && guard < 64; ++guard) {
            if (scope_var(L, v)) return ptr_[static_cast<std::size_t>(v)] ? v : -1;
            auto* s = def_[static_cast<std::size_t>(v)];
            if (!s || s->kind != Stmt::Assign || s->args.empty() || s->args[0].is_const) return -1;
            if (!(s->ptr_arith || s->op == Op::Copy)) return -1;
            v = s->args[0].var;
        }
        return -1;
    }
    void leaves(int L, const Arg& a, std::set<int>& out, int depth = 0) const {
        if (a.is_const || depth > 8) return;
        if (scope_var(L, a.var)) {
            if (!ptr_[static_cast<std::size_t>(a.var)]) out.insert(a.var);
            return;
        }
        auto* s = def_[static_cast<std::size_t>(a.var)];
        if (!s || s->kind != Stmt::Assign) return;
        switch (s->op) {
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Shl: case Op::ZExt: case Op::SExt: case Op::Trunc:
            case Op::Copy:
                for (auto& x : s->args) leaves(L, x, out, depth + 1);
                break;
            default: break;
        }
    }
    void generate(std::vector<InvCand>& out);
    void generate_generic(std::vector<InvCand>& out);
    void prepare_idioms();
    bool idiom_valid(const InvCand& c) const;
    // Write footprint of loop L in cut mode: known objects, else the object
    // of the pointer variable (defined before the loop) the target is
    // derived from, else every object.
    int write_base(int L, const WriteInst& w) const {
        for (int v : w.chain)
            if (!in_loop(L, is_param(v) ? -1 : g_.def_block[static_cast<std::size_t>(v)]))
                return scope_var(L, v) ? v : -1;
        return -1;
    }
    Footprint cut_footprint(int L, const std::vector<WriteInst>& writes) const {
        Footprint fp;
        for (auto& w : writes) {
            if (!in_loop(L, w.block)) continue;
            if (!w.inits) fp.keep_init = false;
            if (w.obj) {
                fp.objs.insert(*w.obj);
                continue;
            }
            int b = write_base(L, w);
            if (b >= 0) fp.base_vars.insert(b);
            else fp.all = true;
        }
        if (fp.all) {
            fp.objs.clear();
            fp.base_vars.clear();
        }
        return fp;
    }
    bool cut_covers(int L, const Footprint& fp, const std::vector<WriteInst>& writes) const {
        for (auto& w : writes) {
            if (!in_loop(L, w.block)) continue;
            if (fp.keep_init && !w.inits) return false;
            if (fp.all) continue;
            if (w.obj && fp.objs.count(*w.obj)) continue;
            int b = w.obj ? -1 : write_base(L, w);
            if (b >= 0 && fp.base_vars.count(b)) continue;
            return false;
        }
        return true;
    }
    std::string term_text(const InvTerm& t) const;
    std::string text(const InvCand& c) const;
    std::optional<nlohmann::json> load_cache();
    void store_cache(HoudiniOut& out);
};

// Cache of Houdini outcomes: <cache_dir>/houdini/<key>.json, key = sha256 of
// the function's PIR text, the memory encoding, the per-query time limit
// and every candidate's text (so a change to the templates or the function
// is a different key).
std::optional<nlohmann::json> Houdini::load_cache() {
    namespace fs = std::filesystem;
    if (env_.cache_dir.empty()) return std::nullopt;
    std::string k = "houdini-cache-v1\n" + to_text(fn_) + "\nmemory=" +
                    (eo_.memory == MemEncoding::Array ? "array" : "bv") + "\ntimeout=" + std::to_string(per_query_) +
                    "\nloops=" + std::to_string(g_.loops.size()) + "\n";
    // the concrete pre-filter changes what a time-limited search reaches
    if (const char* ce = std::getenv("PRISM_HOUDINI_CONCRETE"); ce && std::string(ce) == "1") k += "concrete=v1\n";
    // generate() has run: the candidates are the ones this search starts from
    for (auto& c : cands_for_key_) k += c + "\n";
    const auto h = sha256_hex(k);
    cache_file_ = (fs::path(env_.cache_dir) / "houdini" / (h + ".json")).string();
    std::error_code ec;
    if (!fs::exists(cache_file_, ec)) return std::nullopt;
    try {
        std::ifstream in(cache_file_);
        auto j = nlohmann::json::parse(in);
        if (!j.is_object() || !j.contains("proved")) return std::nullopt;
        if (j.value("proved", false) && !(j.contains("survivors") && j["survivors"].is_array())) return std::nullopt;
        return j;
    } catch (...) {
        return std::nullopt;  // unreadable entry: searched again
    }
}

void Houdini::store_cache(HoudiniOut& out) {
    namespace fs = std::filesystem;
    if (from_cache_) {
        out.cache = out.proved ? "hit (proof re-checked)" : "hit (cached proof not re-established)";
        return;
    }
    if (cache_file_.empty()) return;
    // a "no proof" that a time limit or an unknown answer decided is not kept
    if (!out.proved && out.undecided) return;
    nlohmann::json j;
    j["proved"] = out.proved;
    if (out.proved) {
        nlohmann::json sv = nlohmann::json::array();
        for (auto& k : out.cands)
            if (k.alive) sv.push_back(text(k));
        j["survivors"] = sv;
    } else {
        j["why"] = out.why;
    }
    std::error_code ec;
    fs::create_directories(fs::path(cache_file_).parent_path(), ec);
    const auto tmp = cache_file_ + ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream o(tmp);
        if (!o) return;
        o << j.dump();
    }
    fs::rename(tmp, cache_file_, ec);
    if (ec) fs::remove(tmp, ec);
    else out.cache = "stored";
}

std::string Houdini::term_text(const InvTerm& t) const {
    auto name = [&](int v) { return "%" + fn_.vars[static_cast<std::size_t>(v)].name; };
    switch (t.kind) {
        case InvTerm::Phi: return "phi#" + std::to_string(t.idx);
        case InvTerm::Scope: return name(t.idx);
        case InvTerm::Const: return std::to_string(as_signed(t.c, t.w));
        case InvTerm::Entry: return "entry(phi#" + std::to_string(t.idx) + ")";
        case InvTerm::Size: return "size(" + name(t.idx) + ")";
    }
    return "?";
}

std::string Houdini::text(const InvCand& c) const {
    const auto& H = fn_.blocks[static_cast<std::size_t>(g_.loops[static_cast<std::size_t>(c.loop)].header)];
    auto phi = [&](int i) { return "%" + fn_.vars[static_cast<std::size_t>(H.phis[static_cast<std::size_t>(i)].dst)].name; };
    auto tt = [&](const InvTerm& t) {
        if (t.kind == InvTerm::Phi) return phi(t.idx);
        if (t.kind == InvTerm::Entry) return "entry(" + phi(t.idx) + ")";
        return term_text(t);
    };
    auto var = [&](int v) { return "%" + fn_.vars[static_cast<std::size_t>(v)].name; };
    std::string idx = (c.sext ? "sext(" : "zext(") + (c.ip >= 0 ? phi(c.ip) : std::string("?")) + ")";
    std::string range = std::string(c.lo_entry ? "entry(" + phi(c.ip) + ") <= x < " : "x < ") + idx;
    std::string eb = std::to_string(c.eb);
    auto at = [&](int p, int off) {
        return var(p) + "[" + (off >= 0 ? var(off) + " + " : std::string()) + "x]" + (c.eb > 1 ? "/" + eb : "");
    };
    switch (c.kind) {
        case InvCand::Cmp: return tt(c.a) + " " + op_name(c.rel) + " " + tt(c.b);
        case InvCand::Mask:
            return "(" + tt(c.a) + " & " + std::to_string(c.mask) + ") == (" + tt(c.b) + " & " +
                   std::to_string(c.mask) + ")";
        case InvCand::Lin: {
            const char* o = c.sum ? " + " : " - ";
            return tt(c.a) + o + tt(c.b) + " == entry(" + phi(c.a.idx) + ")" + o + "entry(" + phi(c.b.idx) + ")";
        }
        case InvCand::MaxLe:
            return tt(c.a) + (c.sext ? " sle" : " ule") + " max(entry(" + phi(c.a.idx) + "), " + tt(c.b) + ")";
        case InvCand::Scale:
            return tt(c.a) + " - entry(" + phi(c.a.idx) + ") " + op_name(c.rel) + " " +
                   std::to_string(as_signed(c.mask, c.a.w)) + " * (" + tt(c.b) + " - entry(" + phi(c.b.idx) +
                   "))" + (c.sext ? " [int]" : " [nat]");
        case InvCand::ScaleV:
            return tt(c.a) + " - entry(" + phi(c.a.idx) + ") == " + tt(c.v) + " * (" + tt(c.b) + " - entry(" +
                   phi(c.b.idx) + "))" + (c.sext ? " [int]" : " [nat]");
        case InvCand::Tri: {
            const std::string d = std::to_string(static_cast<int>(c.mask) - 1);
            return "2 * (" + tt(c.a) + " - entry(" + phi(c.a.idx) + ")) == " + tt(c.b) + " * (" + tt(c.b) + " + " + d +
                   ") - entry(" + phi(c.b.idx) + ") * (entry(" + phi(c.b.idx) + ") + " + d + ")" +
                   (c.sext ? " [int]" : " [nat]");
        }
        case InvCand::DiffCmp: return "(" + tt(c.a) + " - " + tt(c.b) + ") " + op_name(c.rel) + " " + tt(c.v);
        case InvCand::ScaleVK:
            return tt(c.a) + " - entry(" + phi(c.a.idx) + ") " + op_name(c.rel) + " " + std::to_string(c.mask) + " * " +
                   tt(c.v) + " * (" + tt(c.b) + " - entry(" + phi(c.b.idx) + "))" + (c.sext ? " [int]" : " [nat]");
        case InvCand::ModLin:
            return "((" + tt(c.a) + " - " + tt(c.b) + " - " + tt(c.v) + ") & " + std::to_string(c.mask) + ") == ((entry(" +
                   phi(c.a.idx) + ") - entry(" + phi(c.b.idx) + ") - entry(" + phi(c.v.idx) + ")) & " +
                   std::to_string(c.mask) + ")";
        case InvCand::MemRingInit:
            return "forall x < " + std::to_string(c.mask) + " with (x - " + phi(c.ip) + ") mod " + std::to_string(c.mask) +
                   " < " + phi(c.ip2) + ": initialised " + var(c.p) + "[x]" + (c.eb > 1 ? "/" + eb : "");
        case InvCand::MemScale:
            return "forall x < " + std::to_string(c.mask) + ": " + var(c.p) + "[x]" + (c.eb > 1 ? "/" + eb : "") + " " +
                   op_name(c.rel) + " " + std::to_string(static_cast<int64_t>(c.v.c)) + " * (" + phi(c.ip2) +
                   " - entry(" + phi(c.ip2) + "))" + (c.sext ? " [int]" : " [nat]");
        case InvCand::MemNZ: return "forall " + range + ": " + at(c.p, -1) + " != 0";
        case InvCand::MemEq: return "forall " + range + ": " + at(c.p, c.off) + " == " + at(c.q, -1);
        case InvCand::MemNeC: return "forall " + range + ": " + at(c.p, -1) + " != (i" + std::to_string(8 * c.eb) + ")" + tt(c.v);
        case InvCand::MemInit: return "forall " + range + ": initialised " + at(c.p, c.off);
        case InvCand::MemConst: return "forall " + range + ": " + at(c.p, -1) + " == " + tt(c.v);
        case InvCand::PtrIn:
            return phi(c.rp) + " == 0 || " + phi(c.rp) + " in " + var(c.p) + "[0, " + idx + ")";
        case InvCand::PtrAt: return phi(c.rp) + " == 0 || *" + phi(c.rp) + " == (i8)" + tt(c.v);
        case InvCand::MemLast:
            return "forall " + range + ": " + at(c.p, -1) + " == (i8)" + tt(c.v) + " -> " + phi(c.rp) + " != 0 && x <= " +
                   phi(c.rp) + " - " + var(c.p);
        case InvCand::MemFrame:
            return "forall x < " + (c.off >= 0 ? var(c.off) + " + " : std::string()) + (c.sext ? "sext" : "zext") +
                   "(entry(" + phi(c.ip) + ")): " + at(c.p, -1) +
                   " == " + at(c.p, -1) + " at loop entry";
        case InvCand::MemScopeAt: {
            // off < 0 is "no displacement" (x itself), not variable -1.
            const std::string quant = c.sum ? "x <= " + idx : range;
            if (c.off < 0) return "forall " + quant + ": x < size(" + var(c.p) + ")";
            if (c.sum) return "forall x <= " + idx + ": " + var(c.off) + " + x < size(" + var(c.p) + ")";
            return "forall " + range + ": " + var(c.off) + " + x < size(" + var(c.p) + ")";
        }
    }
    return "?";
}

void Houdini::generate(std::vector<InvCand>& out) {
    if (idiom_round_) {  // the idioms' candidates alone
        out = idiom_cands_;
        return;
    }
    generate_generic(out);
    // the generic family, then the idioms' candidates it does not have
    std::set<std::string> have;
    for (auto& c : out) have.insert(text(c));
    for (auto& c : idiom_cands_)
        if (have.insert(text(c)).second) out.push_back(c);
}

void Houdini::generate_generic(std::vector<InvCand>& out) {
    constexpr std::size_t kScopeFlat = 16, kConstsFlat = 8, kScopeNested = 6, kConstsNested = 3, kGhostPerLoop = 160, kSteps = 4, kArithPhis = 6, kArithScope = 4;
    static const Op kRel[] = {Op::Eq, Op::Ne, Op::Ule, Op::Ult, Op::Uge, Op::Ugt,
                              Op::Sle, Op::Slt, Op::Sge, Op::Sgt};
    for (std::size_t Li = 0; Li < g_.loops.size(); ++Li) {
        const int L = static_cast<int>(Li);
        const auto& loop = g_.loops[Li];
        const auto& H = fn_.blocks[static_cast<std::size_t>(loop.header)];
        // Nested loops (this loop contains or is contained in another) get a
        // smaller scope and constant set: their candidates multiply across the
        // loop nest and nested_4 needed ~780 MB of a 788 MB budget.
        bool nested = false;
        for (std::size_t Lj = 0; Lj < g_.loops.size() && !nested; ++Lj) {
            if (Lj == Li) continue;
            const auto& o = g_.loops[Lj];
            nested = loop.body[static_cast<std::size_t>(o.header)] || o.body[static_cast<std::size_t>(loop.header)];
        }
        const std::size_t kScope = nested ? kScopeNested : kScopeFlat;
        const std::size_t kConsts = nested ? kConstsNested : kConstsFlat;
        // header phis (integer, not floating point)
        std::vector<int> phis;
        for (std::size_t i = 0; i < H.phis.size(); ++i) {
            auto& v = fn_.vars[static_cast<std::size_t>(H.phis[i].dst)];
            if (!v.fp && v.width > 1 && v.width <= 64) phis.push_back(static_cast<int>(i));
        }
        if (phis.empty()) continue;
        auto phi_var = [&](int i) { return H.phis[static_cast<std::size_t>(i)].dst; };
        // scope values: parameters, then values defined before the loop, most recent first
        std::vector<int> scope;
        for (int v : fn_.params)
            if (!ptr_[static_cast<std::size_t>(v)] && !fn_.vars[static_cast<std::size_t>(v)].fp &&
                fn_.vars[static_cast<std::size_t>(v)].width > 1)
                scope.push_back(v);
        // values the loop body uses
        std::vector<char> used(fn_.vars.size(), 0);
        for (int b = 0; b < g_.n; ++b) {
            if (!loop.body[static_cast<std::size_t>(b)]) continue;
            auto& bl = fn_.blocks[static_cast<std::size_t>(b)];
            for (auto& p : bl.phis)
                for (auto& [pb, a] : p.in) {
                    (void)pb;
                    if (!a.is_const) used[static_cast<std::size_t>(a.var)] = 1;
                }
            for (auto& st : bl.stmts)
                for (auto& a : st.args)
                    if (!a.is_const) used[static_cast<std::size_t>(a.var)] = 1;
            if (bl.term.kind == Term::Br && !bl.term.cond.is_const) used[static_cast<std::size_t>(bl.term.cond.var)] = 1;
        }
        std::vector<int> later;
        for (int v = static_cast<int>(fn_.vars.size()) - 1; v >= 0; --v) {
            auto& var = fn_.vars[static_cast<std::size_t>(v)];
            if (var.fp || var.width <= 1 || var.width > 64 || ptr_[static_cast<std::size_t>(v)] || is_param(v)) continue;
            if (!scope_var(L, v)) continue;
            if (!var.name.empty() && var.name[0] == '_') continue;  // translator temporaries (checks)
            auto* s = def_[static_cast<std::size_t>(v)];
            if (s && s->kind == Stmt::Assign && s->op >= Op::SAddOvf && s->op <= Op::InexactS) continue;
            // used by the loop, a nondet value, or a value merged before the loop
            const bool merged = !s && g_.def_block[static_cast<std::size_t>(v)] >= 0;
            const bool nondet = s && s->op == Op::Havoc;
            if (!used[static_cast<std::size_t>(v)] && !merged && !nondet) continue;
            later.push_back(v);
        }
        for (int v : later)
            if (scope.size() < kScope) scope.push_back(v);
        // constants of the loop body, 0 and 1
        std::map<unsigned, std::set<uint64_t>> consts;
        std::set<int> nec_vals;             // scope values compared in the loop (strchr's c)
        std::set<uint64_t> store_consts[9]; // by element bytes
        std::set<int> bases_r, bases_w;     // accessed objects
        std::set<int> offs;                 // scope offsets in written addresses
        std::set<unsigned> ebs;
        std::map<int, std::set<bool>> ext;  // phi var -> sext?
        // step constants (k >= 2) of the loop's arithmetic: Scale templates
        std::map<unsigned, std::set<uint64_t>> steps;
        auto note_step = [&](const Arg& a) {
            if (!a.is_const || a.width <= 1 || a.width > 64) return;
            const int64_t sv = as_signed(a.bits, a.width);
            if ((sv >= 2 || sv <= -2) && steps[a.width].size() < kSteps) steps[a.width].insert(a.bits);
        };
        auto note_const = [&](const Arg& a) {
            if (a.is_const && a.width > 1 && a.width <= 64 && consts[a.width].size() < kConsts) consts[a.width].insert(a.bits);
        };
        for (int b = 0; b < g_.n; ++b) {
            if (!loop.body[static_cast<std::size_t>(b)]) continue;
            for (auto& s : fn_.blocks[static_cast<std::size_t>(b)].stmts) {
                if (s.kind == Stmt::Assign) {
                    if ((s.op >= Op::Eq && s.op <= Op::Sge) || s.op == Op::Add || s.op == Op::Sub)
                        for (auto& a : s.args) note_const(a);
                    if (s.op == Op::Add || s.op == Op::Sub || s.op == Op::Mul || s.op == Op::SDiv || s.op == Op::UDiv)
                        for (auto& a : s.args) note_step(a);
                    if ((s.op == Op::Eq || s.op == Op::Ne) && s.args.size() == 2)
                        for (auto& a : s.args) {
                            std::set<int> lv;
                            leaves(L, a, lv);
                            for (int x : lv) nec_vals.insert(x);
                        }
                    if ((s.op == Op::ZExt || s.op == Op::SExt) && !s.args[0].is_const)
                        ext[s.args[0].var].insert(s.op == Op::SExt);
                }
                if (s.kind == Stmt::Load || s.kind == Stmt::Store) {
                    unsigned w = s.kind == Stmt::Load ? fn_.vars[static_cast<std::size_t>(s.dst)].width : s.args[1].width;
                    unsigned bytes = (w + 7) / 8;
                    if (bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8) ebs.insert(bytes);
                    if (s.args[0].is_const) continue;
                    int base = base_of(L, s.args[0].var);
                    if (base < 0) continue;
                    (s.kind == Stmt::Load ? bases_r : bases_w).insert(base);
                    if (s.kind == Stmt::Store) {
                        if (s.args[1].is_const && bytes <= 8) store_consts[bytes].insert(s.args[1].bits);
                        // scope values added to the index of the written address
                        int v = s.args[0].var;
                        for (int guard = 0; v >= 0 && guard < 16; ++guard) {
                            auto* d = def_[static_cast<std::size_t>(v)];
                            if (!d || d->kind != Stmt::Assign || d->args.empty() || d->args[0].is_const) break;
                            if (d->ptr_arith && d->args.size() > 1) {
                                std::set<int> lv;
                                leaves(L, d->args[1], lv);
                                for (int x : lv)
                                    if (fn_.vars[static_cast<std::size_t>(x)].width == ptr_w()) offs.insert(x);
                            }
                            if (!(d->ptr_arith || d->op == Op::Copy)) break;
                            v = d->args[0].var;
                        }
                    }
                }
            }
        }
        // ---- scalar templates
        for (int i : phis) {
            unsigned w = fn_.vars[static_cast<std::size_t>(phi_var(i))].width;
            InvTerm a{InvTerm::Phi, i, 0, w};
            std::vector<InvTerm> bs;
            for (int j : phis)
                if (j > i && fn_.vars[static_cast<std::size_t>(phi_var(j))].width == w) bs.push_back({InvTerm::Phi, j, 0, w});
            for (int v : scope)
                if (fn_.vars[static_cast<std::size_t>(v)].width == w) bs.push_back({InvTerm::Scope, v, 0, w});
            std::set<uint64_t> cs = consts[w];
            cs.insert(0);
            cs.insert(1);
            for (auto c : cs) bs.push_back({InvTerm::Const, -1, c & wmask(w), w});
            if (w == ptr_w())
                for (int p : bases_r)
                    bs.push_back({InvTerm::Size, p, 0, w});
            if (w == ptr_w())
                for (int p : bases_w)
                    if (!bases_r.count(p)) bs.push_back({InvTerm::Size, p, 0, w});
            bs.push_back({InvTerm::Entry, i, 0, w});
            const bool sgn = signed_use_[static_cast<std::size_t>(phi_var(i))] != 0;
            for (auto& b : bs)
                for (Op r : kRel) {
                    // signed relations only for values the program uses as signed
                    if (!sgn && (r == Op::Sle || r == Op::Slt || r == Op::Sge || r == Op::Sgt)) continue;
                    InvCand c;
                    c.kind = InvCand::Cmp;
                    c.loop = L;
                    c.rel = r;
                    c.a = a;
                    c.b = b;
                    out.push_back(c);
                }
            for (uint64_t m : {1u, 3u, 7u}) {
                if (m >= wmask(w)) continue;
                InvCand c;
                c.kind = InvCand::Mask;
                c.loop = L;
                c.a = a;
                c.b = {InvTerm::Entry, i, 0, w};
                c.mask = m;
                out.push_back(c);
            }
            for (int j : phis)
                if (j > i && fn_.vars[static_cast<std::size_t>(phi_var(j))].width == w)
                    for (bool sum : {false, true}) {
                        InvCand c;
                        c.kind = InvCand::Lin;
                        c.loop = L;
                        c.a = a;
                        c.b = {InvTerm::Phi, j, 0, w};
                        c.sum = sum;
                        out.push_back(c);
                    }
            // a <= max(entry(a), b): a counter bounded by a guard value that
            // may lie below its start
            for (int v : scope)
                if (fn_.vars[static_cast<std::size_t>(v)].width == w) {
                    InvCand c;
                    c.kind = InvCand::MaxLe;
                    c.loop = L;
                    c.a = a;
                    c.b = {InvTerm::Scope, v, 0, w};
                    c.sext = sgn;
                    out.push_back(c);
                }
            if (phis.size() > kArithPhis) continue;
            for (int j : phis) {
                if (j == i || fn_.vars[static_cast<std::size_t>(phi_var(j))].width != w) continue;
                const InvTerm b{InvTerm::Phi, j, 0, w};
                const bool sg = sgn || signed_use_[static_cast<std::size_t>(phi_var(j))] != 0;
                for (uint64_t k : steps[w])
                    for (Op r : {Op::Eq, Op::Sle, Op::Sge}) {
                        InvCand c;
                        c.kind = InvCand::Scale;
                        c.loop = L;
                        c.rel = r;
                        c.a = a;
                        c.b = b;
                        c.mask = k & wmask(w);
                        c.sext = sg;
                        out.push_back(c);
                    }
                // Unit-bounded accumulation (kindmem_alloc_true: s += q[1] with
                // q[1] == 1 per iteration): a - entry(a) <= / >= b - entry(b).
                // The Eq form is Lin's; only the inequalities are new.
                for (Op r : {Op::Sle, Op::Sge}) {
                    InvCand c;
                    c.kind = InvCand::Scale;
                    c.loop = L;
                    c.rel = r;
                    c.a = a;
                    c.b = b;
                    c.mask = 1;
                    c.sext = sg;
                    out.push_back(c);
                }
                std::size_t nv = 0;
                for (int v : scope) {
                    if (fn_.vars[static_cast<std::size_t>(v)].width != w || nv >= kArithScope) continue;
                    ++nv;
                    InvCand c;
                    c.kind = InvCand::ScaleV;
                    c.loop = L;
                    c.a = a;
                    c.b = b;
                    c.v = {InvTerm::Scope, v, 0, w};
                    c.sext = sg;
                    out.push_back(c);
                }
                for (uint64_t d1 : {0u, 1u, 2u}) {  // d = -1, 0, +1
                    InvCand c;
                    c.kind = InvCand::Tri;
                    c.loop = L;
                    c.a = a;
                    c.b = b;
                    c.mask = d1;
                    c.sext = sg;
                    out.push_back(c);
                }
            }
        }
        // ---- memory templates (universally quantified over the index x)
        std::size_t ghosts = 0;
        auto add = [&](InvCand c) {
            if (ghosts >= kGhostPerLoop) return;
            ++ghosts;
            out.push_back(std::move(c));
        };
        if (ebs.empty()) continue;
        // a pointer phi that remembers the last match (strrchr): null or a
        // pointer below the index into an accessed object, at the value
        for (int r : phis) {
            int rv = phi_var(r);
            if (fn_.vars[static_cast<std::size_t>(rv)].width != ptr_w() || !ptr_[static_cast<std::size_t>(rv)]) continue;
            for (int i : phis) {
                int iv = phi_var(i);
                if (i == r || ptr_[static_cast<std::size_t>(iv)] || fn_.vars[static_cast<std::size_t>(iv)].width < 8) continue;
                std::set<bool> exts;
                if (fn_.vars[static_cast<std::size_t>(iv)].width == ptr_w()) exts.insert(false);
                else exts = ext[iv];
                for (bool sx : exts)
                    for (int p : bases_r) {
                        InvCand c;
                        c.loop = L;
                        c.rp = r;
                        c.ip = i;
                        c.sext = sx;
                        c.p = p;
                        c.kind = InvCand::PtrIn;
                        add(c);
                        for (int v : nec_vals) {
                            c.v = {InvTerm::Scope, v, 0, fn_.vars[static_cast<std::size_t>(v)].width};
                            c.kind = InvCand::PtrAt;
                            add(c);
                            c.kind = InvCand::MemLast;
                            add(c);
                        }
                    }
            }
        }
        for (int i : phis) {
            int pv = phi_var(i);
            unsigned w = fn_.vars[static_cast<std::size_t>(pv)].width;
            if (w < 8) continue;
            std::set<bool> exts;
            if (w == ptr_w()) exts.insert(false);
            else exts = ext[pv];
            for (bool sx : exts)
                for (unsigned eb : ebs)
                    for (bool lo : {false, true}) {
                        InvCand t;
                        t.loop = L;
                        t.ip = i;
                        t.sext = sx;
                        t.eb = eb;
                        t.lo_entry = lo;
                        std::set<int> all = bases_r;
                        all.insert(bases_w.begin(), bases_w.end());
                        for (int p : all) {
                            t.p = p;
                            if (!lo) {
                                auto c = t;
                                c.kind = InvCand::MemNZ;
                                add(c);
                                if (eb == 1)
                                    for (int v : nec_vals) {
                                        c.kind = InvCand::MemNeC;
                                        c.v = {InvTerm::Scope, v, 0, fn_.vars[static_cast<std::size_t>(v)].width};
                                        add(c);
                                    }
                            }
                            for (auto k : store_consts[eb]) {
                                auto c = t;
                                c.kind = InvCand::MemConst;
                                c.v = {InvTerm::Const, -1, k & wmask(8 * eb), 8 * eb};
                                add(c);
                            }
                        }
                        if (lo) continue;
                        for (int p : all) {
                            t.p = p;
                            const bool wp = bases_w.count(p) != 0;
                            std::vector<int> os{-1};
                            if (wp) os.insert(os.end(), offs.begin(), offs.end());
                            for (int o : os) {
                                auto c = t;
                                c.off = o;
                                if (wp) {
                                    c.kind = InvCand::MemInit;
                                    add(c);
                                }
                                for (int q : all) {
                                    if (q == p || (!wp && !bases_w.count(q) && q < p)) continue;
                                    c.kind = InvCand::MemEq;
                                    c.q = q;
                                    add(c);
                                }
                            }
                            if (wp)  // the bytes below the entry index (+ offset) are the loop entry's
                                for (int o : os) {
                                    auto c = t;
                                    c.kind = InvCand::MemFrame;
                                    c.off = o;
                                    add(c);
                                }
                            // strcat/strncat append loops: bytes written so far, and the index
                            // about to be written (sum: x <= i), stay inside p's object.
                            if (wp)
                                for (int o : offs) {
                                    auto c = t;
                                    c.kind = InvCand::MemScopeAt;
                                    c.off = o;
                                    c.sum = false;
                                    add(c);
                                    c.sum = true;
                                    add(c);
                                }
                            // source reads s[i]. off < 0 means no scope displacement:
                            // forall x < i (and x <= i) : x < size(p). Not a lookup of variable -1.
                            if (!wp && bases_r.count(p) != 0) {
                                auto c = t;
                                c.kind = InvCand::MemScopeAt;
                                c.off = -1;
                                c.sum = false;
                                add(c);
                                c.sum = true;
                                add(c);
                            }
                        }
                    }
        }
    }
}

// ---- concrete pre-filter ----
// The function is run on the PIR interpreter (the semantics translation
// validation compares with the encoder) on deterministic inputs: edge values
// and seeded random ones. At each visit of a loop header, every scalar
// candidate of that loop is evaluated on the real state; one that is false is
// dropped before any query. Sound because it only drops: a candidate false at
// a header state of a violation-free run is false at a state the base or step
// query must admit (the run's states satisfy reach and noviol before the
// header), so it is outside the greatest inductive subset Houdini computes and
// the fixpoint is unchanged; only the SAT queries that would have found it go.
// Only templates evaluated here exactly as `eval` builds them are filtered
// (Cmp, Mask, Lin, MaxLe, and Scale/ScaleV/Tri up to 128 bits); memory
// templates and object sizes are left to the solver. Opt-in: PRISM_HOUDINI_CONCRETE=1.
namespace houdini_concrete {
inline uint64_t mask_w(unsigned w) { return w >= 64 ? ~uint64_t{0} : (uint64_t{1} << w) - 1; }
inline int64_t sgn(uint64_t v, unsigned w) {
    v &= mask_w(w);
    if (w == 0 || w >= 64) return static_cast<int64_t>(v);
    return (v >> (w - 1)) & 1 ? static_cast<int64_t>(v | ~mask_w(w)) : static_cast<int64_t>(v);
}
using u128 = unsigned __int128;
inline u128 mask128(unsigned W) { return W >= 128 ? ~u128{0} : (u128{1} << W) - 1; }
// x (xw bits) extended to W bits, signed or unsigned, as a W-bit pattern
inline u128 ext(uint64_t x, unsigned xw, unsigned W, bool sx) {
    u128 v = x & mask_w(xw);
    if (sx && xw > 0 && xw < 128 && ((v >> (xw - 1)) & 1)) v |= ~mask128(xw);
    return v & mask128(W);
}
inline __int128 s128(u128 v, unsigned W) {
    v &= mask128(W);
    if (W < 128 && ((v >> (W - 1)) & 1)) v |= ~mask128(W);
    return static_cast<__int128>(v);
}
inline uint64_t splitmix(uint64_t& st) {
    uint64_t z = (st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
}  // namespace houdini_concrete

void Houdini::concrete_filter(HoudiniOut& out) {
    namespace hc = houdini_concrete;
    // opt-in until measured on the merge gate (docs/VERIFY_PRISM.md): =1 turns it on
    if (const char* e = std::getenv("PRISM_HOUDINI_CONCRETE"); !(e && std::string(e) == "1")) return;
    auto& cands = out.cands;
    if (g_.loops.empty() || cands.empty()) return;
    const auto nb = fn_.blocks.size();
    // widths of a term as the encoder sees it (0: not evaluated here)
    auto header_of = [&](int L) { return g_.loops[static_cast<std::size_t>(L)].header; };
    auto phi_w = [&](int L, int idx) -> unsigned {
        const int h = header_of(L);
        if (h < 0 || static_cast<std::size_t>(h) >= nb) return 0;
        auto& ph = fn_.blocks[static_cast<std::size_t>(h)].phis;
        if (idx < 0 || static_cast<std::size_t>(idx) >= ph.size()) return 0;
        return fn_.vars[static_cast<std::size_t>(ph[static_cast<std::size_t>(idx)].dst)].width;
    };
    auto term_w = [&](const InvTerm& t, int L) -> unsigned {
        switch (t.kind) {
            case InvTerm::Phi:
            case InvTerm::Entry: return phi_w(L, t.idx);
            case InvTerm::Scope:
                if (t.idx < 0 || static_cast<std::size_t>(t.idx) >= fn_.vars.size()) return 0;
                return fn_.vars[static_cast<std::size_t>(t.idx)].width == t.w ? t.w : 0;
            case InvTerm::Const: return t.w;
            case InvTerm::Size: return 0;
        }
        return 0;
    };
    auto known_rel = [](Op r) {
        switch (r) {
            case Op::Eq: case Op::Ne: case Op::Ule: case Op::Ult: case Op::Uge: case Op::Ugt:
            case Op::Sle: case Op::Slt: case Op::Sge: case Op::Sgt: return true;
            default: return false;
        }
    };
    auto evaluable = [&](const InvCand& k) -> bool {
        if (k.loop < 0 || static_cast<std::size_t>(k.loop) >= g_.loops.size()) return false;
        const unsigned wa = term_w(k.a, k.loop), wb = term_w(k.b, k.loop);
        if (wa == 0 || wa > 64) return false;
        switch (k.kind) {
            case InvCand::Cmp: return wb == wa && known_rel(k.rel);
            case InvCand::Mask: return wb == wa;
            case InvCand::Lin: return wb == wa && k.a.kind == InvTerm::Phi && k.b.kind == InvTerm::Phi;
            case InvCand::MaxLe: return wb == wa && k.a.kind == InvTerm::Phi;
            case InvCand::Scale:
            case InvCand::Tri:
                return 2 * wa + 4 <= 128 && wb > 0 && wb <= 64 && k.a.kind == InvTerm::Phi && k.b.kind == InvTerm::Phi &&
                       (k.kind == InvCand::Tri || k.rel == Op::Eq || k.rel == Op::Sle || k.rel == Op::Sge);
            case InvCand::ScaleV: {
                const unsigned wv = term_w(k.v, k.loop);
                return 2 * wa + 4 <= 128 && wb > 0 && wb <= 64 && wv > 0 && wv <= 64 && k.a.kind == InvTerm::Phi &&
                       k.b.kind == InvTerm::Phi;
            }
            default: return false;
        }
    };
    std::vector<std::vector<std::size_t>> by_loop(g_.loops.size());
    std::size_t todo = 0;
    for (std::size_t i = 0; i < cands.size(); ++i)
        if (cands[i].alive && evaluable(cands[i])) {
            by_loop[static_cast<std::size_t>(cands[i].loop)].push_back(i);
            ++todo;
        }
    if (todo == 0) return;
    std::map<int, std::vector<int>> loops_at;  // header block -> loops
    for (std::size_t L = 0; L < g_.loops.size(); ++L) loops_at[g_.loops[L].header].push_back(static_cast<int>(L));
    std::vector<std::vector<uint64_t>> entry(g_.loops.size());
    std::vector<char> have_entry(g_.loops.size(), 0);
    auto holds = [&](const InvCand& k, const std::vector<uint64_t>& ph, const std::vector<uint64_t>& en,
                     const std::vector<uint64_t>& val) -> bool {
        auto tv = [&](const InvTerm& t) -> uint64_t {
            switch (t.kind) {
                case InvTerm::Phi: return ph.at(static_cast<std::size_t>(t.idx));
                case InvTerm::Entry: return en.at(static_cast<std::size_t>(t.idx));
                case InvTerm::Scope: return val.at(static_cast<std::size_t>(t.idx)) & hc::mask_w(t.w);
                case InvTerm::Const: return t.c & hc::mask_w(t.w);
                default: return 0;
            }
        };
        const unsigned w = term_w(k.a, k.loop);
        const uint64_t M = hc::mask_w(w);
        const uint64_t a = tv(k.a) & M;
        switch (k.kind) {
            case InvCand::Cmp: {
                const uint64_t b = tv(k.b) & M;
                const int64_t sa = hc::sgn(a, w), sb = hc::sgn(b, w);
                switch (k.rel) {
                    case Op::Eq: return a == b;
                    case Op::Ne: return a != b;
                    case Op::Ule: return a <= b;
                    case Op::Ult: return a < b;
                    case Op::Uge: return a >= b;
                    case Op::Ugt: return a > b;
                    case Op::Sle: return sa <= sb;
                    case Op::Slt: return sa < sb;
                    case Op::Sge: return sa >= sb;
                    case Op::Sgt: return sa > sb;
                    default: return true;
                }
            }
            case InvCand::Mask: {
                const uint64_t m = k.mask & M, b = tv(k.b) & M;
                return (a & m) == (b & m);
            }
            case InvCand::Lin: {
                const uint64_t b = tv(k.b) & M, ea = en.at(static_cast<std::size_t>(k.a.idx)) & M,
                               eb = en.at(static_cast<std::size_t>(k.b.idx)) & M;
                return k.sum ? ((a + b) & M) == ((ea + eb) & M) : ((a - b) & M) == ((ea - eb) & M);
            }
            case InvCand::MaxLe: {
                const uint64_t b = tv(k.b) & M, ea = en.at(static_cast<std::size_t>(k.a.idx)) & M;
                if (k.sext) {
                    const int64_t sa = hc::sgn(a, w), sb = hc::sgn(b, w), sea = hc::sgn(ea, w);
                    return sa <= (sea >= sb ? sea : sb);
                }
                return a <= (ea >= b ? ea : b);
            }
            case InvCand::Scale:
            case InvCand::ScaleV:
            case InvCand::Tri: {
                const unsigned W = 2 * w + 4, wb = term_w(k.b, k.loop);
                const hc::u128 WM = hc::mask128(W);
                auto X = [&](uint64_t x, unsigned xw) { return hc::ext(x, xw, W, k.sext); };
                const hc::u128 A = X(a, w), B = X(tv(k.b), wb);
                const hc::u128 EA = X(en.at(static_cast<std::size_t>(k.a.idx)), w);
                const hc::u128 EB = X(en.at(static_cast<std::size_t>(k.b.idx)), wb);
                if (k.kind == InvCand::Tri) {
                    const hc::u128 D = static_cast<hc::u128>(static_cast<__int128>(static_cast<int>(k.mask) - 1)) & WM;
                    const hc::u128 lhs = (hc::u128{2} * (A - EA)) & WM;
                    const hc::u128 rhs = (B * (B + D) - EB * (EB + D)) & WM;
                    return lhs == rhs;
                }
                hc::u128 m;
                if (k.kind == InvCand::Scale)
                    m = k.sext ? static_cast<hc::u128>(static_cast<__int128>(hc::sgn(k.mask, w))) & WM
                               : static_cast<hc::u128>(k.mask) & WM;
                else
                    m = X(tv(k.v), term_w(k.v, k.loop));
                const hc::u128 lhs = (A - EA) & WM, rhs = (m * (B - EB)) & WM;
                if (k.rel == Op::Sle) return hc::s128(lhs, W) <= hc::s128(rhs, W);
                if (k.rel == Op::Sge) return hc::s128(lhs, W) >= hc::s128(rhs, W);
                return lhs == rhs;
            }
            default: return true;
        }
    };
    InterpHook hook = [&](int b, int prev, const std::vector<uint64_t>& val) -> bool {
        auto it = loops_at.find(b);
        if (it == loops_at.end()) return true;
        auto& hb = fn_.blocks[static_cast<std::size_t>(b)];
        std::vector<uint64_t> ph(hb.phis.size());
        for (std::size_t j = 0; j < hb.phis.size(); ++j) ph[j] = val.at(static_cast<std::size_t>(hb.phis[j].dst));
        for (int L : it->second) {
            auto& lp = g_.loops[static_cast<std::size_t>(L)];
            const bool from_inside = prev >= 0 && static_cast<std::size_t>(prev) < lp.body.size() &&
                                     lp.body[static_cast<std::size_t>(prev)];
            if (!from_inside) {
                entry[static_cast<std::size_t>(L)] = ph;
                have_entry[static_cast<std::size_t>(L)] = 1;
            }
            if (!have_entry[static_cast<std::size_t>(L)]) continue;
            for (auto i : by_loop[static_cast<std::size_t>(L)]) {
                if (!cands[i].alive) continue;
                if (!holds(cands[i], ph, entry[static_cast<std::size_t>(L)], val)) {
                    cands[i].alive = false;
                    ++out.concrete_dropped;
                    if (--todo == 0) return false;  // nothing left to falsify
                }
            }
        }
        return true;
    };
    // deterministic inputs: edge values, then seeded random ones
    std::vector<unsigned> pw;
    for (int p : fn_.params) pw.push_back(fn_.vars[static_cast<std::size_t>(p)].width);
    uint64_t seed = 0x5eed0f1a7e5ull;
    const prism::clock::Stopwatch t0;
    constexpr int kRuns = 48;
    constexpr double kWall = 1.5;  // seconds for the whole filter
    for (int run = 0; run < kRuns && todo > 0 && t0.elapsed_s() < kWall; ++run) {
        std::vector<uint64_t> args(pw.size());
        for (std::size_t j = 0; j < pw.size(); ++j) {
            const unsigned w = pw[j];
            const uint64_t M = hc::mask_w(w);
            uint64_t v = 0;
            switch (run) {
                case 0: v = 0; break;
                case 1: v = 1; break;
                case 2: v = M; break;  // -1
                case 3: v = w ? (M >> 1) : 0; break;  // signed max
                case 4: v = w ? (uint64_t{1} << (w - 1)) : 0; break;  // signed min
                case 5: v = 2; break;
                default: {
                    const uint64_t r = hc::splitmix(seed);
                    switch (r % 4) {
                        case 0: v = r >> 60; break;           // small: 0..15
                        case 1: v = (r >> 32) % 300; break;   // a few hundred
                        case 2: v = ~((r >> 58)) ; break;     // small negative
                        default: v = hc::splitmix(seed); break;
                    }
                }
            }
            args[j] = v & M;
        }
        for (auto& en : have_entry) en = 0;
        (void)interpret(fn_, args, 200'000, hook);
        ++out.concrete_runs;
    }
}

// Loop idioms (src/prism/pir/idioms/idioms.hpp): the library matches each
// loop's shape and instantiates its templates; they become candidates like
// any other (nothing about them is assumed).
InvCand from_idiom(const idioms::TCand& t) {
    auto tm = [](const idioms::TTerm& x) {
        InvTerm r;
        switch (x.kind) {
            case idioms::TTerm::Phi: r.kind = InvTerm::Phi; break;
            case idioms::TTerm::Scope: r.kind = InvTerm::Scope; break;
            case idioms::TTerm::Const: r.kind = InvTerm::Const; break;
            case idioms::TTerm::Entry: r.kind = InvTerm::Entry; break;
            case idioms::TTerm::Size: r.kind = InvTerm::Size; break;
        }
        r.idx = x.idx;
        r.c = x.c;
        r.w = x.w;
        return r;
    };
    InvCand c;
    c.loop = t.loop;
    c.rel = t.rel;
    c.a = tm(t.a);
    c.b = tm(t.b);
    c.v = tm(t.v);
    c.mask = t.mask;
    c.sext = t.sext;
    c.p = t.p;
    c.ip = t.ip;
    c.ip2 = t.ip2;
    c.eb = t.eb;
    switch (t.kind) {
        case idioms::TCand::Cmp: c.kind = InvCand::Cmp; break;
        case idioms::TCand::MaxLe: c.kind = InvCand::MaxLe; break;
        case idioms::TCand::Scale: c.kind = InvCand::Scale; break;
        case idioms::TCand::ScaleV: c.kind = InvCand::ScaleV; break;
        case idioms::TCand::DiffCmp: c.kind = InvCand::DiffCmp; break;
        case idioms::TCand::ScaleVK: c.kind = InvCand::ScaleVK; break;
        case idioms::TCand::ModLin: c.kind = InvCand::ModLin; break;
        case idioms::TCand::MemInit: c.kind = InvCand::MemInit; break;
        case idioms::TCand::MemConst: c.kind = InvCand::MemConst; break;
        case idioms::TCand::MemRingInit: c.kind = InvCand::MemRingInit; break;
        case idioms::TCand::MemScale: c.kind = InvCand::MemScale; break;
    }
    return c;
}

// Widths and indices an idiom candidate must have to be encoded (a z3 sort
// error would end the function's check, not drop the candidate).
bool Houdini::idiom_valid(const InvCand& c) const {
    if (c.loop < 0 || static_cast<std::size_t>(c.loop) >= g_.loops.size()) return false;
    const auto& H = fn_.blocks[static_cast<std::size_t>(g_.loops[static_cast<std::size_t>(c.loop)].header)];
    auto phi_w = [&](int i) -> unsigned {
        if (i < 0 || static_cast<std::size_t>(i) >= H.phis.size()) return 0;
        auto& v = fn_.vars[static_cast<std::size_t>(H.phis[static_cast<std::size_t>(i)].dst)];
        return v.fp ? 0 : v.width;
    };
    auto tw = [&](const InvTerm& t) -> unsigned {
        switch (t.kind) {
            case InvTerm::Phi:
            case InvTerm::Entry: return phi_w(t.idx) == t.w ? t.w : 0;
            case InvTerm::Scope: {
                if (t.idx < 0 || static_cast<std::size_t>(t.idx) >= fn_.vars.size()) return 0;
                auto& v = fn_.vars[static_cast<std::size_t>(t.idx)];
                return v.width == t.w && !v.fp && scope_var(c.loop, t.idx) ? t.w : 0;
            }
            case InvTerm::Const: return t.w;
            case InvTerm::Size: return 0;
        }
        return 0;
    };
    auto ptr_scope = [&](int p) {
        return p >= 0 && static_cast<std::size_t>(p) < fn_.vars.size() &&
               fn_.vars[static_cast<std::size_t>(p)].width == ptr_w() && scope_var(c.loop, p);
    };
    auto eb_ok = [&] { return c.eb == 1 || c.eb == 2 || c.eb == 4 || c.eb == 8; };
    auto idx_ok = [&](int i) { return phi_w(i) >= 8 && phi_w(i) <= ptr_w(); };
    const unsigned wa = tw(c.a);
    const bool a_phi = wa > 0 && wa <= 64 && c.a.kind == InvTerm::Phi;
    switch (c.kind) {
        case InvCand::Cmp:
        case InvCand::MaxLe: return a_phi && tw(c.b) == wa;
        case InvCand::Scale: return a_phi && c.b.kind == InvTerm::Phi && tw(c.b) > 0 && tw(c.b) <= 2 * wa + 4;
        case InvCand::ScaleV:
            return a_phi && c.b.kind == InvTerm::Phi && tw(c.b) > 0 && tw(c.b) <= 2 * wa + 4 && tw(c.v) > 0 &&
                   tw(c.v) <= 2 * wa + 4;
        case InvCand::ScaleVK:
            return a_phi && c.b.kind == InvTerm::Phi && tw(c.b) > 0 && tw(c.b) <= wa && tw(c.v) > 0 && tw(c.v) <= wa &&
                   (c.rel == Op::Eq || c.rel == Op::Sle || c.rel == Op::Sge);
        case InvCand::DiffCmp: return a_phi && c.b.kind == InvTerm::Phi && tw(c.b) == wa && tw(c.v) == wa;
        case InvCand::ModLin:
            return a_phi && c.b.kind == InvTerm::Phi && c.v.kind == InvTerm::Phi && tw(c.b) == wa && tw(c.v) == wa;
        case InvCand::MemInit: return eb_ok() && idx_ok(c.ip) && ptr_scope(c.p) && c.off < 0 && !c.lo_entry;
        case InvCand::MemConst:
            return eb_ok() && idx_ok(c.ip) && ptr_scope(c.p) && c.v.kind == InvTerm::Const && c.v.w == 8 * c.eb &&
                   !c.lo_entry;
        case InvCand::MemRingInit:
            return eb_ok() && idx_ok(c.ip) && idx_ok(c.ip2) && ptr_scope(c.p) && c.mask > 0 && c.mask < (uint64_t{1} << 32);
        case InvCand::MemScale:
            return eb_ok() && idx_ok(c.ip) && phi_w(c.ip2) > 0 && phi_w(c.ip2) <= 64 && ptr_scope(c.p) && c.mask > 0 &&
                   c.mask < (uint64_t{1} << 32) && c.v.kind == InvTerm::Const && c.v.w == 64 &&
                   (c.rel == Op::Sle || c.rel == Op::Sge);
        default: return false;
    }
}

void Houdini::prepare_idioms() {
    std::vector<idioms::LoopView> lv;
    for (auto& L : g_.loops) lv.push_back({L.header, L.body});
    idioms::Context ctx;
    ctx.scope = [this](int L, int v) { return scope_var(L, v); };
    ctx.signed_use = [this](int v) {
        return v >= 0 && static_cast<std::size_t>(v) < signed_use_.size() && signed_use_[static_cast<std::size_t>(v)] != 0;
    };
    for (auto& t : idioms::instantiate(fn_, lv, ctx)) {
        auto c = from_idiom(t);
        if (!idiom_valid(c)) continue;
        if (idiom_by_text_.emplace(text(c), t.idiom).second) idiom_cands_.push_back(c);
    }
}

HoudiniOut Houdini::run() {
    ptr_ = pointer_vars(fn_);
    def_.assign(fn_.vars.size(), nullptr);
    for (auto& b : fn_.blocks)
        for (auto& s : b.stmts)
            if (s.kind == Stmt::Assign && s.dst >= 0) def_[static_cast<std::size_t>(s.dst)] = &s;
    signed_use_.assign(fn_.vars.size(), 0);
    for (auto& b : fn_.blocks)
        for (auto& st : b.stmts) {
            if (st.kind != Stmt::Assign) continue;
            switch (st.op) {
                case Op::Slt: case Op::Sle: case Op::Sgt: case Op::Sge: case Op::SAddOvf: case Op::SSubOvf:
                case Op::SMulOvf: case Op::SDiv: case Op::SRem: case Op::SDivOvf: case Op::AShr: case Op::SExt:
                case Op::SMax: case Op::SMin: case Op::ShlSOvf: case Op::ShlNswOvf:
                    for (auto& a : st.args)
                        if (!a.is_const) signed_use_[static_cast<std::size_t>(a.var)] = 1;
                    break;
                default: break;
            }
        }
    prepare_idioms();
    // $PRISM_PIR_IDIOMS=0: the generic family alone (measurements)
    static const bool idioms_on = [] {
        const char* e = std::getenv("PRISM_PIR_IDIOMS");
        return !(e && std::string(e) == "0");
    }();
    if (!idioms_on) {
        idiom_cands_.clear();
        idiom_by_text_.clear();
    }
    if (idiom_cands_.empty()) return run_search();
    auto names_of = [&](const HoudiniOut& o) {
        std::set<std::string> ns;
        for (auto& k : o.cands)
            if (k.alive)
                if (auto it = idiom_by_text_.find(text(k)); it != idiom_by_text_.end()) ns.insert(it->second);
        std::string r;
        for (auto& n : ns) r += (r.empty() ? "" : ",") + n;
        return r.empty() ? std::string("none") : r;
    };
    std::set<std::string> all;
    for (auto& [tx, n] : idiom_by_text_) all.insert(n);
    std::string matched;
    for (auto& n : all) matched += (matched.empty() ? "" : ",") + n;
    // the idiom round: its candidates alone, with a share of the search time
    const double saved = deadline_;
    const double share = std::max(per_query_, 0.3 * std::max(0.0, saved - t0_.elapsed_s()));
    deadline_ = std::min(saved, t0_.elapsed_s() + share);
    idiom_round_ = true;
    // a template that is not decided quickly is dropped (an idiom's
    // invariants are simple when they hold): a quarter of the query time
    const double saved_q = per_query_;
    per_query_ = std::max(1.0, per_query_ / 4);
    const prism::clock::Stopwatch t1;
    HoudiniOut first = run_search();
    const double first_s = t1.elapsed_s();
    idiom_round_ = false;
    deadline_ = saved;
    per_query_ = saved_q;
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.2f", first_s);
    if (first.proved) {
        first.idiom = names_of(first) + ": closed by the idiom round (" + std::to_string(first.candidates) +
                      " candidate(s), " + secs + " s; matched: " + matched + ")";
        return first;
    }
    // the generic family with the idioms' candidates added
    from_cache_ = false;
    cache_file_.clear();
    cands_for_key_.clear();
    HoudiniOut second = run_search();
    second.idiom = matched + ": the idiom round did not close (" + std::to_string(first.candidates) + " candidate(s), " +
                   secs + " s: " + first.why + ")" +
                   (second.proved ? "; closed by the generic search (idiom invariants kept: " + names_of(second) + ")"
                                  : std::string());
    return second;
}

HoudiniOut Houdini::run_search() {
    HoudiniOut out;
    out.attempted = true;
    generate(out.cands);
    out.candidates = out.cands.size();
    out.invariants.assign(g_.loops.size(), {});
    out.kept.assign(g_.loops.size(), {});
    // An earlier run's outcome for the same function text, encoding, time
    // limit and candidates: "no proof" (decided without a time limit or an
    // unknown answer) is taken as it is; a proof restricts the candidates to
    // its survivors, which are then checked again from scratch (base, step
    // and the final query: a cached proof is never taken on trust).
    for (auto& k : out.cands) cands_for_key_.push_back(text(k));
    if (auto hit = load_cache()) {
        if (!hit->value("proved", false)) {
            out.why = "no proof (outcome of an earlier run, cached: " + hit->value("why", std::string("?")) + ")";
            out.cache = "hit (no proof)";
            return out;
        }
        std::set<std::string> keep;
        for (auto& x : (*hit)["survivors"]) keep.insert(x.get<std::string>());
        for (auto& k : out.cands) k.alive = keep.count(text(k)) != 0;
        from_cache_ = true;
    }
    if (!from_cache_) concrete_filter(out);

    // footprints: learnt from a first cut encoding, then re-checked (the
    // havoc can change what provenance knows about a write target)
    std::vector<Footprint> fps(g_.loops.size());
    for (int attempt = 0;; ++attempt) {
        z3::context c;
        // the in-process Houdini checks bypass the portfolio's z3-worker
        // child (z3_worker_argv): stop them at the budget watchdog's
        // trip; an interrupted check is unknown, never unsat
        membudget::Watch mem_watch([&c] { Z3_interrupt(c); });
        Encoding e(c, fn_, g_, 1, eo_);
        e.cut_mode = true;
        e.loop_fp = fps;
        e.cut_loops.assign(g_.loops.size(), {});
        e.build();
        bool ok = true;
        for (std::size_t L = 0; L < g_.loops.size(); ++L) {
            if (cut_covers(static_cast<int>(L), fps[L], e.writes)) continue;
            ok = false;
            auto more = cut_footprint(static_cast<int>(L), e.writes);
            if (attempt < 2) {
                fps[L].objs.insert(more.objs.begin(), more.objs.end());
                fps[L].base_vars.insert(more.base_vars.begin(), more.base_vars.end());
                fps[L].all = fps[L].all || more.all;
                fps[L].keep_init = fps[L].keep_init && more.keep_init;
                if (fps[L].all) {
                    fps[L].objs.clear();
                    fps[L].base_vars.clear();
                }
            } else {
                fps[L].all = true;
                fps[L].objs.clear();
                fps[L].base_vars.clear();
                fps[L].keep_init = fps[L].keep_init && more.keep_init;
            }
        }
        if (ok) break;
        if (attempt == 4) {
            out.why = "loop write footprint not stable";
            return out;
        }
    }
    {
        std::vector<std::string> fs;
        for (std::size_t L = 0; L < fps.size(); ++L)
            if (!fps[L].empty())
                fs.push_back("loop " + std::to_string(L) + ": " +
                             (fps[L].all ? std::string("every object")
                                         : std::to_string(fps[L].objs.size() + fps[L].base_vars.size()) + " object(s)"));
        out.footprint = fs.empty() ? "read-only loops" : join_s(fs, "; ");
    }

    z3::context c;
    membudget::Watch mem_watch([&c] { Z3_interrupt(c); });  // see the footprint loop above
    Encoding e(c, fn_, g_, 1, eo_);
    e.cut_mode = true;
    e.loop_fp = fps;
    e.cut_loops.assign(g_.loops.size(), {});
    e.build();
    for (std::size_t L = 0; L < g_.loops.size(); ++L)
        if (!cut_covers(static_cast<int>(L), fps[L], e.writes)) {
            out.why = "loop write footprint not covered";
            return out;
        }

    auto& cands = out.cands;
    const z3::expr x0 = c.bv_const("inv!x", ptr_w());
    auto ext64 = [&](const z3::expr& v, bool sx) {
        unsigned w = v.get_sort().bv_size();
        const unsigned pw = ptr_w();
        if (w == pw) return v;
        if (w > pw) return v.extract(pw - 1, 0);  // a candidate only; admitted when proved
        return sx ? z3::sext(v, pw - w) : z3::zext(v, pw - w);
    };
    // index terms at which the universal templates are assumed
    std::vector<z3::expr> T{x0};
    std::set<unsigned> seen{x0.id()};
    auto addT = [&](const z3::expr& t) {
        if (T.size() < 24 && seen.insert(t.id()).second) T.push_back(t);
    };
    for (auto& cand : cands)
        if (cand.ghost()) {
            auto& CL = e.cut_loops[static_cast<std::size_t>(cand.loop)];
            if (CL.node >= 0) addT(ext64(CL.havoc[static_cast<std::size_t>(cand.ip)], cand.sext));
        }
    for (auto& pe : e.params) {
        if (pe.get_sort().bv_size() < 8) continue;
        addT(ext64(pe, false));
        addT(ext64(pe, true));
    }
    // In the idiom round: the element indices the loops load and store
    // (p[(tail & 31)] is read at an index no header phi holds). An instance
    // of a proved universal at any term is one more true fact.
    if (idiom_round_)
        for (std::size_t n = 0; n < e.nodes.size() && T.size() < 24; ++n) {
            const int b = e.nodes[n].block;
            bool in_some = false;
            for (std::size_t L = 0; L < g_.loops.size() && !in_some; ++L) in_some = in_loop(static_cast<int>(L), b);
            if (!in_some) continue;
            for (auto& st : fn_.blocks[static_cast<std::size_t>(b)].stmts) {
                if ((st.kind != Stmt::Load && st.kind != Stmt::Store) || st.args.empty() || st.args[0].is_const) continue;
                auto* d = def_[static_cast<std::size_t>(st.args[0].var)];
                if (!d || !d->ptr_arith || d->op != Op::Add || d->args.size() != 2 || d->args[1].is_const) continue;
                Arg ix = d->args[1];
                if (auto* m = def_[static_cast<std::size_t>(ix.var)];
                    m && m->op == Op::Mul && m->args.size() == 2 && m->args[1].is_const && !m->args[0].is_const)
                    ix = m->args[0];
                if (ix.width != ptr_w()) continue;
                try {
                    addT(e.lookup(ix, static_cast<int>(n)));
                } catch (const EncodeFail&) {
                }
            }
        }
    // MemEq is p[off + x] == q[x] and MemFrame frames bytes below off. An
    // assertion that reads p[j] for a parameter j is the instance x = j - off
    // (strcat/strncat: d[k + (j - k)]). One more index of a proved universal
    // does not weaken it; the quantifier guard still drops x that are out of range.
    for (auto& cand : cands) {
        if ((cand.kind != InvCand::MemEq && cand.kind != InvCand::MemFrame) || cand.off < 0) continue;
        auto& CL = e.cut_loops[static_cast<std::size_t>(cand.loop)];
        if (CL.node < 0) continue;
        try {
            z3::expr off = e.lookup(Arg::v(cand.off, ptr_w()), CL.node);
            if (!off.is_bv() || off.get_sort().bv_size() != ptr_w()) continue;
            for (auto& pe : e.params) {
                unsigned w = pe.get_sort().bv_size();
                if (w < 8 || w > ptr_w()) continue;
                addT((w == ptr_w() ? pe : z3::zext(pe, ptr_w() - w)) - off);
            }
        } catch (const EncodeFail&) {
            continue;
        }
    }

    struct State {
        const std::vector<z3::expr>* phis;
        const mem::SymMem::Mark* mem;
    };
    auto term = [&](const InvTerm& t, const State& st, int node, int L) -> z3::expr {
        switch (t.kind) {
            case InvTerm::Phi: return (*st.phis)[static_cast<std::size_t>(t.idx)];
            case InvTerm::Scope: return e.lookup(Arg::v(t.idx, t.w), node);
            case InvTerm::Const: return e.bv(t.c, t.w);
            case InvTerm::Entry: return e.cut_loops[static_cast<std::size_t>(L)].entry[static_cast<std::size_t>(t.idx)];
            case InvTerm::Size: return e.M().size(e.lookup(Arg::v(t.idx, ptr_w()), node));
        }
        return e.bv(0, 1);
    };
    auto rel = [&](Op r, const z3::expr& a, const z3::expr& b) -> z3::expr {
        switch (r) {
            case Op::Eq: return a == b;
            case Op::Ne: return a != b;
            case Op::Ule: return z3::ule(a, b);
            case Op::Ult: return z3::ult(a, b);
            case Op::Uge: return z3::uge(a, b);
            case Op::Ugt: return z3::ugt(a, b);
            case Op::Sle: return a <= b;
            case Op::Slt: return a < b;
            case Op::Sge: return a >= b;
            case Op::Sgt: return a > b;
            default: return c.bool_val(false);
        }
    };
    auto eval = [&](const InvCand& k, const State& st, const z3::expr& x) -> z3::expr {
        auto& CL = e.cut_loops[static_cast<std::size_t>(k.loop)];
        const int node = CL.node;
        switch (k.kind) {
            case InvCand::Cmp: return rel(k.rel, term(k.a, st, node, k.loop), term(k.b, st, node, k.loop));
            case InvCand::Mask: {
                auto m = e.bv(k.mask, k.a.w);
                return (term(k.a, st, node, k.loop) & m) == (term(k.b, st, node, k.loop) & m);
            }
            case InvCand::Lin: {
                auto a = term(k.a, st, node, k.loop), b = term(k.b, st, node, k.loop);
                auto ea = CL.entry[static_cast<std::size_t>(k.a.idx)], eb = CL.entry[static_cast<std::size_t>(k.b.idx)];
                return k.sum ? a + b == ea + eb : a - b == ea - eb;
            }
            case InvCand::MaxLe: {
                auto a = term(k.a, st, node, k.loop), b = term(k.b, st, node, k.loop);
                auto ea = CL.entry[static_cast<std::size_t>(k.a.idx)];
                if (k.sext) return a <= z3::ite(ea >= b, ea, b);
                return z3::ule(a, z3::ite(z3::uge(ea, b), ea, b));
            }
            case InvCand::Scale:
            case InvCand::ScaleV:
            case InvCand::Tri: {
                const unsigned w = k.a.w, W = 2 * w + 4;
                auto X = [&](const z3::expr& x) {
                    const unsigned xw = x.get_sort().bv_size();
                    return k.sext ? z3::sext(x, W - xw) : z3::zext(x, W - xw);
                };
                auto a = X(term(k.a, st, node, k.loop)), b = X(term(k.b, st, node, k.loop));
                auto ea = X(CL.entry[static_cast<std::size_t>(k.a.idx)]);
                auto eb = X(CL.entry[static_cast<std::size_t>(k.b.idx)]);
                if (k.kind == InvCand::Tri) {
                    auto d = c.bv_val(static_cast<int>(k.mask) - 1, W);
                    return c.bv_val(2, W) * (a - ea) == b * (b + d) - eb * (eb + d);
                }
                auto m = k.kind == InvCand::Scale
                             ? (k.sext ? c.bv_val(static_cast<int64_t>(as_signed(k.mask, w)), W) : c.bv_val(k.mask, W))
                             : X(term(k.v, st, node, k.loop));
                auto lhs = a - ea, rhs = m * (b - eb);
                if (k.rel == Op::Sle) return lhs <= rhs;
                if (k.rel == Op::Sge) return lhs >= rhs;
                return lhs == rhs;
            }
            case InvCand::DiffCmp:
                return rel(k.rel, term(k.a, st, node, k.loop) - term(k.b, st, node, k.loop), term(k.v, st, node, k.loop));
            case InvCand::ScaleVK: {
                const unsigned W = 3 * k.a.w + 4;
                auto X = [&](const z3::expr& x) {
                    const unsigned xw = x.get_sort().bv_size();
                    return k.sext ? z3::sext(x, W - xw) : z3::zext(x, W - xw);
                };
                auto a = X(term(k.a, st, node, k.loop)), b = X(term(k.b, st, node, k.loop));
                auto ea = X(CL.entry[static_cast<std::size_t>(k.a.idx)]);
                auto eb = X(CL.entry[static_cast<std::size_t>(k.b.idx)]);
                auto lhs = a - ea, rhs = c.bv_val(k.mask, W) * X(term(k.v, st, node, k.loop)) * (b - eb);
                if (k.rel == Op::Sle) return lhs <= rhs;
                if (k.rel == Op::Sge) return lhs >= rhs;
                return lhs == rhs;
            }
            case InvCand::ModLin: {
                auto m = e.bv(k.mask, k.a.w);
                auto now = term(k.a, st, node, k.loop) - term(k.b, st, node, k.loop) - term(k.v, st, node, k.loop);
                auto was = CL.entry[static_cast<std::size_t>(k.a.idx)] - CL.entry[static_cast<std::size_t>(k.b.idx)] -
                           CL.entry[static_cast<std::size_t>(k.v.idx)];
                return (now & m) == (was & m);
            }
            case InvCand::MemRingInit: {
                const unsigned pw = ptr_w();
                auto zx = [&](const z3::expr& v) {
                    const unsigned w = v.get_sort().bv_size();
                    return w == pw ? v : w > pw ? v.extract(pw - 1, 0) : z3::zext(v, pw - w);
                };
                auto t = zx((*st.phis)[static_cast<std::size_t>(k.ip)]), n = zx((*st.phis)[static_cast<std::size_t>(k.ip2)]);
                auto cap = e.bv(k.mask, pw);
                auto dist = z3::ite(z3::uge(x, t), x - t, x + cap - t);
                auto at = e.lookup(Arg::v(k.p, pw), node) + (k.eb == 1 ? x : x * e.bv(k.eb, pw));
                return z3::implies(z3::ult(x, cap) && z3::ult(dist, n),
                                   e.M().load_exact(*st.mem, at, 8 * k.eb).uninit == e.bv(0, 1));
            }
            case InvCand::MemScale: {
                const unsigned pw = ptr_w(), ew = 8 * k.eb;
                const unsigned cw = (*st.phis)[static_cast<std::size_t>(k.ip2)].get_sort().bv_size();
                const unsigned W = 64 + std::max(ew, cw) + 4;
                auto X = [&](const z3::expr& v) {
                    const unsigned w = v.get_sort().bv_size();
                    return k.sext ? z3::sext(v, W - w) : z3::zext(v, W - w);
                };
                auto at = e.lookup(Arg::v(k.p, pw), node) + (k.eb == 1 ? x : x * e.bv(k.eb, pw));
                auto elt = X(e.M().load_exact(*st.mem, at, ew).val);
                auto n = X((*st.phis)[static_cast<std::size_t>(k.ip2)]) - X(CL.entry[static_cast<std::size_t>(k.ip2)]);
                auto kk = c.bv_val(static_cast<int64_t>(k.v.c), W);
                auto body = k.rel == Op::Sle ? elt <= kk * n : elt >= kk * n;
                return z3::implies(z3::ult(x, e.bv(k.mask, pw)), body);
            }
            default: break;
        }
        if (k.kind == InvCand::PtrIn || k.kind == InvCand::PtrAt) {
            auto r = (*st.phis)[static_cast<std::size_t>(k.rp)];
            auto null = r == e.bv(0, ptr_w());
            if (k.kind == InvCand::PtrIn) {
                auto p = e.lookup(Arg::v(k.p, ptr_w()), node);
                auto idx = ext64((*st.phis)[static_cast<std::size_t>(k.ip)], k.sext);
                return null || (r.extract(63, obj_shift()) == p.extract(63, obj_shift()) && z3::uge(r, p) && z3::ult(r - p, idx));
            }
            auto v = e.lookup(Arg::v(k.v.idx, k.v.w), node);
            if (k.v.w > 8) v = v.extract(7, 0);
            else if (k.v.w < 8) v = z3::zext(v, 8 - k.v.w);
            return null || e.M().load_exact(*st.mem, r, 8).val == v;
        }
        // memory templates
        auto idx = ext64((*st.phis)[static_cast<std::size_t>(k.ip)], k.sext);
        z3::expr in = z3::ult(x, idx);
        if (k.kind == InvCand::MemScopeAt && k.sum) in = z3::ule(x, idx);
        if (k.kind == InvCand::MemFrame) {
            z3::expr hi = ext64(CL.entry[static_cast<std::size_t>(k.ip)], k.sext);
            if (k.off >= 0) hi = hi + e.lookup(Arg::v(k.off, ptr_w()), node);
            in = z3::ult(x, hi);
        }
        if (k.lo_entry) in = in && z3::ule(ext64(CL.entry[static_cast<std::size_t>(k.ip)], k.sext), x);
        auto scale = k.eb == 1 ? x : x * e.bv(k.eb, ptr_w());
        auto ptr = [&](int p, int off) {
            z3::expr a = e.lookup(Arg::v(p, ptr_w()), node);
            if (off >= 0) a = a + e.lookup(Arg::v(off, ptr_w()), node);
            return a + scale;
        };
        auto ld = [&](int p, int off) { return e.M().load_exact(*st.mem, ptr(p, off), 8 * k.eb); };
        z3::expr body = c.bool_val(true);
        switch (k.kind) {
            case InvCand::MemNZ: body = ld(k.p, -1).val != e.bv(0, 8 * k.eb); break;
            case InvCand::MemEq: body = ld(k.p, k.off).val == ld(k.q, -1).val; break;
            case InvCand::MemInit: body = ld(k.p, k.off).uninit == e.bv(0, 1); break;
            case InvCand::MemConst: body = ld(k.p, -1).val == e.bv(k.v.c, 8 * k.eb); break;
            case InvCand::MemScopeAt: {
                auto sz = e.M().size(e.lookup(Arg::v(k.p, ptr_w()), node));
                z3::expr at = scale;
                if (k.off >= 0) at = e.lookup(Arg::v(k.off, ptr_w()), node) + scale;
                body = z3::ult(at, sz);
                break;
            }
            case InvCand::MemFrame: {
                auto now = ld(k.p, -1);
                auto was = e.M().load_exact(CL.entry_mem, ptr(k.p, -1), 8 * k.eb);
                body = now.val == was.val && now.uninit == was.uninit;
                break;
            }
            case InvCand::MemLast: {
                auto v = e.lookup(Arg::v(k.v.idx, k.v.w), node);
                if (k.v.w > 8) v = v.extract(7, 0);
                else if (k.v.w < 8) v = z3::zext(v, 8 - k.v.w);
                auto r = (*st.phis)[static_cast<std::size_t>(k.rp)];
                auto p = e.lookup(Arg::v(k.p, ptr_w()), node);
                body = z3::implies(ld(k.p, -1).val == v, r != e.bv(0, ptr_w()) && z3::ule(x, r - p));
                break;
            }
            case InvCand::MemNeC: {
                auto v = e.lookup(Arg::v(k.v.idx, k.v.w), node);
                unsigned w = 8 * k.eb;
                if (k.v.w > w) v = v.extract(w - 1, 0);
                else if (k.v.w < w) v = z3::zext(v, w - k.v.w);
                body = ld(k.p, -1).val != v;
                break;
            }
            default: break;
        }
        return z3::implies(in, body);
    };

    // evaluate every candidate: entry, havoc (instances), next per latch
    struct Ev {
        std::optional<z3::expr> entry;
        std::vector<z3::expr> havoc;
        std::vector<z3::expr> next;
    };
    std::vector<Ev> ev(cands.size());
    for (std::size_t i = 0; i < cands.size(); ++i) {
        auto& k = cands[i];
        auto& CL = e.cut_loops[static_cast<std::size_t>(k.loop)];
        if (CL.node < 0) {
            k.alive = false;
            continue;
        }
        try {
            State se{&CL.entry, &CL.entry_mem}, sh{&CL.havoc, &CL.havoc_mem};
            ev[i].entry = eval(k, se, x0);
            // at x0 only while searching; the other instances are added for
            // the final query (fewer reads keep the search queries small)
            ev[i].havoc.push_back(eval(k, sh, x0));
            for (auto& lt : CL.latches) {
                State sn{&lt.next, &lt.mem};
                ev[i].next.push_back(eval(k, sn, x0));
            }
        } catch (const EncodeFail&) {
            k.alive = false;  // a scope value not readable at the header
        }
    }
    const std::size_t nprops = e.props.size();
    std::vector<std::optional<z3::expr>> noviol_memo(nprops + 1);
    auto noviol = [&](std::size_t n) -> z3::expr {
        if (noviol_memo[n]) return *noviol_memo[n];
        z3::expr_vector v(c);
        for (std::size_t i = 0; i < n; ++i) v.push_back(!e.props[i].viol);
        noviol_memo[n] = v.empty() ? c.bool_val(true) : z3::mk_and(v);
        return *noviol_memo[n];
    };
    // One incremental solver. Candidate i is assumed at its loop's havocked
    // header through a selector literal sel[i]: sel[i] -> (guard -> H_i),
    // guard = the header is reached with no property violated before it.
    // A query passes the selectors of the surviving candidates of the loops
    // it may assume, a literal for its context and one for its goals.
    z3::solver s(c);
    add_all(s, e.assumptions);
    const std::size_t search_assumptions = e.assumptions.size();
    std::vector<std::optional<z3::expr>> sel(cands.size());
    std::vector<z3::expr> sel_impl;  // the selector implications (also given to the final solver)
    for (std::size_t i = 0; i < cands.size(); ++i) {
        if (!cands[i].alive) continue;
        auto& CL = e.cut_loops[static_cast<std::size_t>(cands[i].loop)];
        sel[i] = c.bool_const(("houdini!s" + std::to_string(i)).c_str());
        z3::expr_vector hv(c);
        for (auto& h : ev[i].havoc) hv.push_back(h);
        sel_impl.push_back(z3::implies(
            *sel[i], z3::implies(e.reach[static_cast<std::size_t>(CL.node)] && noviol(CL.props_before), z3::mk_and(hv))));
        s.add(sel_impl.back());
    }
    auto selectors = [&](const std::function<bool(std::size_t)>& before, z3::expr_vector& as) {
        for (std::size_t i = 0; i < cands.size(); ++i)
            if (cands[i].alive && e.cut_loops[static_cast<std::size_t>(cands[i].loop)].node >= 0 &&
                before(static_cast<std::size_t>(cands[i].loop)))
                as.push_back(*sel[i]);
    };
    std::map<int, z3::expr> ctx_lit;
    // A query is asked again only when a candidate it assumes or checks was
    // dropped since it last came back UNSAT with nothing dropped.
    std::vector<int> version(e.cut_loops.size(), 0);
    std::map<int, std::vector<int>> proved_at;
    auto check_goal = [&](const std::function<bool(std::size_t)>& before, const z3::expr& at,
                          const std::function<std::optional<z3::expr>(std::size_t)>& goal_of, int L,
                          int qid) -> bool {
        // returns true when something was dropped
        std::vector<int> deps(e.cut_loops.size(), -1);
        for (std::size_t M = 0; M < e.cut_loops.size(); ++M)
            if (before(M) || static_cast<int>(M) == L) deps[M] = version[M];
        if (auto it = proved_at.find(qid); it != proved_at.end() && it->second == deps) return false;
        std::vector<std::size_t> who;
        for (std::size_t i = 0; i < cands.size(); ++i) {
            if (!cands[i].alive || cands[i].loop != L) continue;
            if (!goal_of(i)) continue;
            who.push_back(i);
        }
        if (who.empty()) return false;
        auto cl = ctx_lit.find(qid);
        if (cl == ctx_lit.end()) {
            cl = ctx_lit.emplace(qid, c.bool_const(("houdini!c" + std::to_string(qid)).c_str())).first;
            s.add(z3::implies(cl->second, at));
        }
        // Drop every goal a model falsifies and ask again for the rest, with
        // the same assumed set (it may still hold candidates dropped here: a
        // model of a stronger context is a counterexample for the weaker one
        // too, so a goal it falsifies is rightly dropped; the outer rounds
        // re-check the survivors with the final set).
        z3::expr_vector base(c);
        base.push_back(cl->second);
        selectors(before, base);
        bool dropped = false;
        // goals: drop what models falsify until the rest is UNSAT; a group
        // without an answer is split in halves, a single goal without an
        // answer is dropped (nothing unproved is kept)
        std::function<void(std::vector<std::size_t>, int)> solve = [&](std::vector<std::size_t> goals, int depth) {
            for (;;) {
                z3::expr_vector rest(c);
                std::vector<std::size_t> live;
                for (auto i : goals)
                    if (cands[i].alive) {
                        rest.push_back(!*goal_of(i));
                        live.push_back(i);
                    }
                if (live.empty()) return;
                if (left() <= 0) {  // out of time: drop them
                    for (auto i : live) cands[i].alive = false;
                    dropped = true;
                    out.undecided = true;
                    return;
                }
                z3::expr act = c.bool_const(("houdini!q" + std::to_string(query_++)).c_str());
                s.add(z3::implies(act, z3::mk_or(rest)));
                z3::expr_vector as(c);
                for (auto x : base) as.push_back(x);
                as.push_back(act);
                // a group gets a share of the time; a single goal the full query time
                double t = live.size() > 1 ? std::min(per_query_, std::max(2.0, per_query_ / 3)) : per_query_;
                z3::params p(c);
                p.set("timeout", query_ms(std::max(1.0, std::min(t, left()))));
                s.set(p);
                const prism::clock::Stopwatch tq0;
                auto r = s.check(as);
                std::optional<z3::model> qm;
                if (r == z3::sat) qm = s.get_model();
                // the activation literal is never assumed again: retire its clause
                // (after the model is read: adding a clause discards it)
                s.add(!act);
                if (houdini_trace())
                    std::fprintf(stderr, "houdini q%d loop %d goals %zu -> %s %.2fs\n", qid, L, live.size(),
                                 r == z3::unsat ? "unsat" : r == z3::sat ? "sat" : "unknown",
                                 tq0.elapsed_s());
                if (houdini_trace() && live.size() == 1 && r != z3::unsat)
                    std::fprintf(stderr, "houdini   goal: %s\n", text(cands[live[0]]).c_str());
                if (r == z3::unsat) return;
                if (r == z3::sat) {
                    auto& m = *qm;
                    bool any = false;
                    for (auto i : live)
                        if (!m.eval(*goal_of(i), true).is_true()) {
                            cands[i].alive = false;
                            any = true;
                        }
                    if (!any) {  // model evaluation disagrees: drop all (never keep an unproved one)
                        for (auto i : live) cands[i].alive = false;
                        out.undecided = true;
                    }
                    dropped = true;
                    continue;
                }
                if (live.size() == 1 || depth > 6) {
                    for (auto i : live) cands[i].alive = false;
                    dropped = true;
                    out.undecided = true;
                    return;
                }
                std::vector<std::size_t> lo(live.begin(), live.begin() + static_cast<std::ptrdiff_t>(live.size() / 2));
                std::vector<std::size_t> hi(live.begin() + static_cast<std::ptrdiff_t>(live.size() / 2), live.end());
                solve(lo, depth + 1);
                solve(hi, depth + 1);
                return;
            }
        };
        solve(who, 0);
        if (dropped) ++version[static_cast<std::size_t>(L)];
        else proved_at[qid] = deps;
        return dropped;
    };
    // The final query: the cut program with the given candidates assumed at
    // every havocked header (the universal ones also at the other index
    // terms).
    // Asked for all properties at once; a group without an answer is split
    // in halves (a smaller query). 1: some property is violated (SAT),
    // 0: none (UNSAT), -1: no answer.
    // Instances at the other index terms, built once per candidate, with
    // the side constraints their reads added (Ackermann constraints of
    // havoc reads: facts of the encoding, true in every query).
    struct Inst {
        bool built = false;
        std::optional<z3::expr> fact;
        std::size_t side_lo = 0, side_hi = 0;  // e.assumptions[side_lo, side_hi)
    };
    std::vector<Inst> inst(cands.size());
    auto instance = [&](std::size_t i) -> const Inst& {
        auto& in = inst[i];
        if (in.built) return in;
        in.built = true;
        in.side_lo = e.assumptions.size();
        auto& CL = e.cut_loops[static_cast<std::size_t>(cands[i].loop)];
        State sh{&CL.havoc, &CL.havoc_mem};
        z3::expr_vector hv(c);
        bool ok = true;
        try {
            for (std::size_t t = 1; t < T.size(); ++t) hv.push_back(eval(cands[i], sh, T[t]));
        } catch (const EncodeFail&) {
            ok = false;  // not assumed at the other terms (only weaker)
        }
        in.side_hi = e.assumptions.size();
        if (ok && !hv.empty())
            in.fact = z3::implies(*sel[i], z3::implies(e.reach[static_cast<std::size_t>(CL.node)] &&
                                                           noviol(CL.props_before),
                                                       z3::mk_and(hv)));
        return in;
    };
    // The universal candidates alive now, instantiated in solver `to`
    // (each candidate once per solver).
    auto add_instances = [&](z3::solver& to, std::vector<char>& added) {
        for (std::size_t i = 0; i < cands.size(); ++i) {
            if (!cands[i].alive || !cands[i].ghost() || added[i]) continue;
            added[i] = 1;
            const auto& in = instance(i);
            for (std::size_t k = in.side_lo; k < in.side_hi; ++k) to.add(e.assumptions[k]);
            if (in.fact) to.add(*in.fact);
        }
    };
    // The optimistic queries go to their own solver, so the instances of
    // candidates that are dropped later never reach the search solver; the
    // final query at the fixpoint is asked in the search solver (which has
    // learnt from the base and step queries) with the survivors' instances
    // only, as before.
    std::optional<z3::solver> sopt;
    std::vector<char> added_opt(cands.size(), 0), added_final(cands.size(), 0);
    auto opt_solver = [&]() -> z3::solver& {
        if (!sopt) {
            sopt.emplace(c);
            for (std::size_t k = 0; k < search_assumptions; ++k) sopt->add(e.assumptions[k]);
            for (auto& x : sel_impl) sopt->add(x);
        }
        add_instances(*sopt, added_opt);
        return *sopt;
    };
    std::string fail;
    auto final_query = [&](double budget, bool split) -> int {
        // split: the final query (search solver); else an optimistic one
        z3::solver& sf = split ? s : opt_solver();
        if (split) add_instances(s, added_final);
        const prism::clock::Stopwatch tf;
        auto fleft = [&] {
            return budget - tf.elapsed_s();
        };
        std::function<int(std::size_t, std::size_t)> prop_group = [&](std::size_t lo, std::size_t hi) -> int {
            if (lo >= hi) return 0;
            if (z3_over_soft_cap()) {
                fail = "cut program: encoder memory over the soft cap (PRISM_PIR_MEM_MB)";
                return -1;
            }
            if (fleft() <= 0) {
                fail = "cut program: out of time";
                return -1;
            }
            z3::expr_vector vs(c);
            for (std::size_t i = lo; i < hi; ++i) vs.push_back(e.props[i].viol);
            z3::expr act = c.bool_const(("houdini!f" + std::to_string(query_++)).c_str());
            sf.add(z3::implies(act, z3::mk_or(vs)));
            z3::params p(c);
            double t = !split ? fleft()
                       : hi - lo == e.props.size() ? std::max(per_query_, fleft() / 2)
                                                   : std::min(per_query_, fleft());
            p.set("timeout", query_ms(std::max(1.0, t)));
            sf.set(p);
            // only the invariants of loops whose header is encoded before
            // some property of the group (a property encoded earlier cannot
            // use them); assuming fewer candidates is only weaker
            z3::expr_vector as(c);
            selectors([&](std::size_t L) { return e.cut_loops[L].props_before < hi; }, as);
            as.push_back(act);
            auto r = sf.check(as);
            if (r == z3::unsat) return 0;
            if (r == z3::sat) {
                fail = "a property is not implied by the inductive invariants found";
                if (houdini_trace()) {  // which property instance the model violates
                    auto m = sf.get_model();
                    for (std::size_t i = lo; i < hi; ++i)
                        if (m.eval(e.props[i].viol, true).is_true()) {
                            std::fprintf(stderr, "houdini   violated: %s line %d (%s)\n", e.props[i].stmt->prop.c_str(),
                                         e.props[i].stmt->line, e.props[i].stmt->msg.c_str());
                            break;
                        }
                }
                return 1;
            }
            if (!split || hi - lo == 1) {
                fail = "cut program: solver unknown";
                return -1;
            }
            std::size_t mid = lo + (hi - lo) / 2;
            if (!prop_group) return -1;
            int a = prop_group(lo, mid);
            if (a != 0) return a;
            return prop_group(mid, hi);
        };
        if (!prop_group) return -1;
        return prop_group(0, e.props.size());
    };
    auto alive_set = [&]() {
        std::vector<char> a(cands.size(), 0);
        for (std::size_t i = 0; i < cands.size(); ++i) a[i] = cands[i].alive ? 1 : 0;
        return a;
    };
    // Optimistic final query: the cut program with every candidate still
    // alive assumed (a superset of the survivors Houdini can end with).
    // Assuming fewer candidates only allows more runs, so a property that
    // is violated here is violated with the fixpoint too: no proof can come
    // out of this search, and it stops (only an early "no proof"; a proof
    // is only ever read off the final query at the fixpoint). UNSAT is
    // remembered: when nothing is dropped afterwards it is the final query.
    std::optional<std::vector<char>> unsat_with;
    int precheck_unknown = 0;
    auto precheck = [&]() -> bool {  // true: stop, no proof possible
        // two without an answer: this function's cut program is too hard
        // for the short query, later ones would only take time
        if (left() <= 0 || precheck_unknown >= 2) return false;
        ++out.prechecks;
        const double t = std::min(left(), std::min(per_query_, std::max(2.0, per_query_ / 3)));
        auto snapshot = alive_set();
        const int r = final_query(t, false);
        if (r == 1) {
            fail = "a property is violated even with every candidate that holds on loop entry assumed";
            return true;
        }
        if (r == 0) unsat_with = std::move(snapshot);
        if (r == -1) ++precheck_unknown;
        fail.clear();
        return false;
    };
    // loops in encoding (topological) order: a base query assumes the loops before it
    std::vector<std::size_t> topo;
    for (std::size_t L = 0; L < e.cut_loops.size(); ++L) topo.push_back(L);
    std::sort(topo.begin(), topo.end(),
              [&](std::size_t a, std::size_t b) { return e.cut_loops[a].seq < e.cut_loops[b].seq; });
    auto base_of_loop = [&](std::size_t L) {
        auto& CL = e.cut_loops[L];
        const int seqL = CL.seq;
        return check_goal([&](std::size_t M) { return e.cut_loops[M].seq < seqL; },
                          e.reach[static_cast<std::size_t>(CL.node)] && noviol(CL.props_before),
                          [&](std::size_t i) { return ev[i].entry; }, static_cast<int>(L), static_cast<int>(L) * 1000);
    };
    auto finish_out = [&](bool proved) {
        for (std::size_t i = 0; i < cands.size(); ++i)
            if (cands[i].alive) {
                out.invariants[static_cast<std::size_t>(cands[i].loop)].push_back(text(cands[i]));
                out.kept[static_cast<std::size_t>(cands[i].loop)].push_back(i);
            }
        out.proved = proved;
        if (!proved) out.why = fail;
        store_cache(out);
        return out;
    };
    // The base queries first (they are cheap and drop every candidate that
    // does not even hold on loop entry, e.g. one of `i == 0` / `i != 0`),
    // then the optimistic query.
    for (std::size_t L : topo)
        if (e.cut_loops[L].node >= 0) base_of_loop(L);
    if (precheck()) return finish_out(false);
    bool changed = true;
    while (changed) {
        if (left() <= 0) {
            out.undecided = true;
            fail = mem_out_ ? "Houdini stopped: encoder memory over the soft cap (PRISM_PIR_MEM_MB)" : "Houdini ran out of time";
            return finish_out(false);
        }
        ++out.rounds;
        changed = false;
        for (std::size_t L : topo) {
            auto& CL = e.cut_loops[L];
            if (CL.node < 0) continue;
            // base: the entry state
            changed |= base_of_loop(L);
            // step: the next state of every back edge
            for (std::size_t j = 0; j < CL.latches.size(); ++j) {
                auto& lt = CL.latches[j];
                const int seqT = lt.seq;
                changed |= check_goal([&](std::size_t M) { return e.cut_loops[M].seq <= seqT; },
                                      lt.guard && noviol(lt.props_before),
                                      [&](std::size_t i) -> std::optional<z3::expr> { return ev[i].next[j]; },
                                      static_cast<int>(L), static_cast<int>(L) * 1000 + 1 + static_cast<int>(j));
            }
        }
        if (changed && precheck()) return finish_out(false);
    }
    // The fixpoint: every survivor is inductive. The function is proved when
    // the cut program with them assumed violates no property (already known
    // when the last optimistic query had exactly these candidates alive).
    if (unsat_with && *unsat_with == alive_set()) return finish_out(true);
    // the final query keeps a minimum of its own (the run's Houdini budget
    // does not cut it), but never runs past the function budget
    // with a hard cap the final query gets all of what is left of it (the
    // search stopped at its deadline, short of the cap by the reserve)
    double final_budget =
        hard_s_ >= 0 ? hard_left() : std::min(std::max(3 * per_query_, left()), hard_left());
    // the idiom round keeps the rest for the generic search
    if (idiom_round_) final_budget = std::min(std::max(8 * per_query_, left()), hard_left());  // per_query_ is a quarter there
    out.final_s = final_budget;
    // the optimistic solver is not asked again: hand its memory back before
    // the final query (nested loops: both together crossed the budget)
    sopt.reset();
    membudget::release_free_memory();
    const int r = final_query(final_budget, true);
    if (r == -1) out.undecided = true;
    return finish_out(r == 0);
}

// Source-level export of the surviving invariants (SV-COMP correctness
// witnesses, tools/svcomp). Only proved invariants of the function's own
// loops (not an inlined callee's) with a source start position, and only
// the scalar ones whose terms are program values (header phis and values
// defined before the loop, by their IR names) or constants:
//   extra["invariant_loops"]      [{"line", "column", "kind": ""}]
//   extra["invariant_conjuncts"]  per loop, [{"rel", "a", "b"}] with terms
//       {"v": IR name, "w": width} or {"c": value (unsigned decimal), "w"};
//       rel is one of eq ne ule ult uge ugt sle slt sge sgt, or
//       "mask:M" ((a & M) == b) / "add" (a + b == c) / "sub" (a - b == c)
// The C text is made by the wrapper, which knows the C types of the names
// (debug information) and exports a conjunct only where C's semantics of the
// expression is the proved bit-vector relation.
void export_invariants(const Function& fn, const CfgInfo& g, const HoudiniOut& h, Verdict& v) {
    nlohmann::json loops = nlohmann::json::array(), conj = nlohmann::json::array();
    auto exportable_name = [](const std::string& n) {
        return !n.empty() && n.find('#') == std::string::npos && n[0] != '_' && n.find('.') != 0;
    };
    for (std::size_t L = 0; L < g.loops.size() && L < h.kept.size(); ++L) {
        const int H = g.loops[L].header;
        auto loc = fn.loop_locs.find(H);
        if (loc == fn.loop_locs.end() || loc->second.first <= 0) continue;
        const auto& hb = fn.blocks[static_cast<std::size_t>(H)];
        // the phi's value on entry, when it is one constant
        auto entry_const = [&](int i) -> std::optional<uint64_t> {
            std::optional<uint64_t> k;
            for (auto& [pb, a] : hb.phis[static_cast<std::size_t>(i)].in) {
                if (g.loops[L].body[static_cast<std::size_t>(pb)]) continue;
                if (!a.is_const || (k && *k != a.bits)) return std::nullopt;
                k = a.bits;
            }
            return k;
        };
        auto term = [&](const InvTerm& t) -> std::optional<nlohmann::json> {
            switch (t.kind) {
                case InvTerm::Phi: {
                    auto& n = fn.vars[static_cast<std::size_t>(hb.phis[static_cast<std::size_t>(t.idx)].dst)].name;
                    if (!exportable_name(n)) return std::nullopt;
                    return nlohmann::json{{"v", n}, {"w", t.w}};
                }
                case InvTerm::Scope: {
                    auto& n = fn.vars[static_cast<std::size_t>(t.idx)].name;
                    if (!exportable_name(n)) return std::nullopt;
                    return nlohmann::json{{"v", n}, {"w", t.w}};
                }
                case InvTerm::Const: return nlohmann::json{{"c", std::to_string(t.c & wmask(t.w))}, {"w", t.w}};
                case InvTerm::Entry: {
                    auto k = entry_const(t.idx);
                    if (!k) return std::nullopt;
                    return nlohmann::json{{"c", std::to_string(*k & wmask(t.w))}, {"w", t.w}};
                }
                case InvTerm::Size: return std::nullopt;
            }
            return std::nullopt;
        };
        nlohmann::json cs = nlohmann::json::array();
        for (auto i : h.kept[L]) {
            const auto& k = h.cands[i];
            auto a = term(k.a);
            if (!a) continue;
            if (k.kind == InvCand::Cmp) {
                auto b = term(k.b);
                if (!b) continue;
                std::string r = op_name(k.rel);
                cs.push_back({{"rel", r}, {"a", *a}, {"b", *b}});
            } else if (k.kind == InvCand::Mask) {
                auto b = term(k.b);
                if (!b || !b->contains("c")) continue;
                uint64_t bits = std::stoull((*b)["c"].get<std::string>()) & k.mask;
                cs.push_back({{"rel", "mask:" + std::to_string(k.mask)},
                              {"a", *a},
                              {"b", {{"c", std::to_string(bits)}, {"w", k.a.w}}}});
            } else if (k.kind == InvCand::Lin) {
                auto b = term(k.b);
                auto ea = entry_const(k.a.idx), eb = entry_const(k.b.idx);
                if (!b || !ea || !eb) continue;
                uint64_t cst = (k.sum ? *ea + *eb : *ea - *eb) & wmask(k.a.w);
                cs.push_back({{"rel", k.sum ? "add" : "sub"},
                              {"a", *a},
                              {"b", *b},
                              {"c", {{"c", std::to_string(cst)}, {"w", k.a.w}}}});
            }
        }
        loops.push_back({{"kind", ""}, {"line", loc->second.first}, {"column", loc->second.second}});
        conj.push_back(cs);
    }
    if (loops.empty()) return;
    v.extra["invariant_loops"] = loops.dump();
    v.extra["invariant_conjuncts"] = conj.dump();
}
