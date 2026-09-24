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

struct InvTerm {
    enum Kind { Phi, Scope, Const, Entry, Size } kind = Const;
    int idx = -1;  // Phi/Entry: phi index in the header; Scope/Size: variable
    uint64_t c = 0;
    unsigned w = 0;
};

struct InvCand {
    enum Kind { Cmp, Mask, Lin, MemNZ, MemEq, MemNeC, MemInit, MemConst } kind = Cmp;
    int loop = -1;
    Op rel = Op::Eq;       // Cmp
    InvTerm a, b;          // Cmp: a rel b; Mask: (a & mask) == (b & mask); Lin: a -/+ b == ea -/+ eb
    bool sum = false;      // Lin: a + b (else a - b)
    uint64_t mask = 0;
    // memory: for every x with lo <= x < ext(phi idx): P(x)
    int ip = -1;           // index phi
    bool sext = false;
    bool lo_entry = false; // lo = entry value of the index phi (else 0)
    int p = -1, q = -1;    // pointers (scope variables)
    int off = -1;          // MemEq/MemInit: p + off + x*eb (scope variable, i64)
    unsigned eb = 1;       // element bytes
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
};

// Pointer-valued variables (allocation results, checked pointer arithmetic,
// copies, selects and phis of those).
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
                bool any = false, all = true;
                for (auto& [pb, a] : p.in) {
                    (void)pb;
                    if (a.is_const) continue;
                    any = true;
                    all = all && is(a);
                }
                if (any && all) set(p.dst);
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
    // timeout_s: the solver time per query; the whole search gets 4 times that
    Houdini(const Function& fn, const CfgInfo& g, const EncodeOptions& eo, double timeout_s)
        : fn_(fn), g_(g), eo_(eo), per_query_(timeout_s), deadline_(4 * timeout_s) {}

    HoudiniOut run();

private:
    const Function& fn_;
    const CfgInfo& g_;
    EncodeOptions eo_;
    double per_query_;
    double deadline_;
    int query_ = 0;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
    std::vector<char> ptr_;
    std::vector<const Stmt*> def_;
    std::vector<int> defb_;

    double left() const {
        return deadline_ - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    }
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
};

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
        case InvCand::MemNZ: return "forall " + range + ": " + at(c.p, -1) + " != 0";
        case InvCand::MemEq: return "forall " + range + ": " + at(c.p, c.off) + " == " + at(c.q, -1);
        case InvCand::MemNeC: return "forall " + range + ": " + at(c.p, -1) + " != (i" + std::to_string(8 * c.eb) + ")" + tt(c.v);
        case InvCand::MemInit: return "forall " + range + ": initialised " + at(c.p, c.off);
        case InvCand::MemConst: return "forall " + range + ": " + at(c.p, -1) + " == " + tt(c.v);
    }
    return "?";
}

void Houdini::generate(std::vector<InvCand>& out) {
    constexpr std::size_t kScope = 16, kConsts = 8, kGhostPerLoop = 160;
    static const Op kRel[] = {Op::Eq, Op::Ne, Op::Ule, Op::Ult, Op::Uge, Op::Ugt,
                              Op::Sle, Op::Slt, Op::Sge, Op::Sgt};
    for (std::size_t Li = 0; Li < g_.loops.size(); ++Li) {
        const int L = static_cast<int>(Li);
        const auto& loop = g_.loops[Li];
        const auto& H = fn_.blocks[static_cast<std::size_t>(loop.header)];
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
        std::vector<int> later;
        for (int v = static_cast<int>(fn_.vars.size()) - 1; v >= 0; --v) {
            auto& var = fn_.vars[static_cast<std::size_t>(v)];
            if (var.fp || var.width <= 1 || var.width > 64 || ptr_[static_cast<std::size_t>(v)] || is_param(v)) continue;
            if (!scope_var(L, v)) continue;
            auto* s = def_[static_cast<std::size_t>(v)];
            if (s && s->kind == Stmt::Assign && s->op >= Op::SAddOvf && s->op <= Op::InexactS) continue;
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
        auto note_const = [&](const Arg& a) {
            if (a.is_const && a.width > 1 && a.width <= 64 && consts[a.width].size() < kConsts) consts[a.width].insert(a.bits);
        };
        for (int b = 0; b < g_.n; ++b) {
            if (!loop.body[static_cast<std::size_t>(b)]) continue;
            for (auto& s : fn_.blocks[static_cast<std::size_t>(b)].stmts) {
                if (s.kind == Stmt::Assign) {
                    if ((s.op >= Op::Eq && s.op <= Op::Sge) || s.op == Op::Add || s.op == Op::Sub)
                        for (auto& a : s.args) note_const(a);
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
                                    if (fn_.vars[static_cast<std::size_t>(x)].width == 64) offs.insert(x);
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
            if (w == 64)
                for (int p : bases_r)
                    bs.push_back({InvTerm::Size, p, 0, w});
            if (w == 64)
                for (int p : bases_w)
                    if (!bases_r.count(p)) bs.push_back({InvTerm::Size, p, 0, w});
            bs.push_back({InvTerm::Entry, i, 0, w});
            for (auto& b : bs)
                for (Op r : kRel) {
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
        }
        // ---- memory templates (universally quantified over the index x)
        std::size_t ghosts = 0;
        auto add = [&](InvCand c) {
            if (ghosts >= kGhostPerLoop) return;
            ++ghosts;
            out.push_back(std::move(c));
        };
        if (ebs.empty()) continue;
        for (int i : phis) {
            int pv = phi_var(i);
            unsigned w = fn_.vars[static_cast<std::size_t>(pv)].width;
            if (w < 8) continue;
            std::set<bool> exts;
            if (w == 64) exts.insert(false);
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
                        for (int p : bases_w) {
                            t.p = p;
                            std::vector<int> os{-1};
                            os.insert(os.end(), offs.begin(), offs.end());
                            for (int o : os) {
                                auto c = t;
                                c.off = o;
                                c.kind = InvCand::MemInit;
                                add(c);
                                for (int q : all) {
                                    if (q == p) continue;
                                    c.kind = InvCand::MemEq;
                                    c.q = q;
                                    add(c);
                                }
                            }
                        }
                    }
        }
    }
}

HoudiniOut Houdini::run() {
    HoudiniOut out;
    out.attempted = true;
    ptr_ = pointer_vars(fn_);
    def_.assign(fn_.vars.size(), nullptr);
    for (auto& b : fn_.blocks)
        for (auto& s : b.stmts)
            if (s.kind == Stmt::Assign && s.dst >= 0) def_[static_cast<std::size_t>(s.dst)] = &s;
    generate(out.cands);
    out.candidates = out.cands.size();
    out.invariants.assign(g_.loops.size(), {});
    out.kept.assign(g_.loops.size(), {});

    // footprints: learnt from a first cut encoding, then re-checked (the
    // havoc can change what provenance knows about a write target)
    std::vector<Footprint> fps(g_.loops.size());
    for (int attempt = 0;; ++attempt) {
        z3::context c;
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
    const z3::expr x0 = c.bv_const("inv!x", 64);
    auto ext64 = [&](const z3::expr& v, bool sx) {
        unsigned w = v.get_sort().bv_size();
        if (w >= 64) return v;
        return sx ? z3::sext(v, 64 - w) : z3::zext(v, 64 - w);
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
            case InvTerm::Size: return e.M().size(e.lookup(Arg::v(t.idx, 64), node));
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
            default: break;
        }
        // memory templates
        auto idx = ext64((*st.phis)[static_cast<std::size_t>(k.ip)], k.sext);
        z3::expr in = z3::ult(x, idx);
        if (k.lo_entry) in = in && z3::ule(ext64(CL.entry[static_cast<std::size_t>(k.ip)], k.sext), x);
        auto scale = k.eb == 1 ? x : x * e.bv(k.eb, 64);
        auto ptr = [&](int p, int off) {
            z3::expr a = e.lookup(Arg::v(p, 64), node);
            if (off >= 0) a = a + e.lookup(Arg::v(off, 64), node);
            return a + scale;
        };
        auto ld = [&](int p, int off) { return e.M().load_exact(*st.mem, ptr(p, off), 8 * k.eb); };
        z3::expr body = c.bool_val(true);
        switch (k.kind) {
            case InvCand::MemNZ: body = ld(k.p, -1).val != e.bv(0, 8 * k.eb); break;
            case InvCand::MemEq: body = ld(k.p, k.off).val == ld(k.q, -1).val; break;
            case InvCand::MemInit: body = ld(k.p, k.off).uninit == e.bv(0, 1); break;
            case InvCand::MemConst: body = ld(k.p, -1).val == e.bv(k.v.c, 8 * k.eb); break;
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
            if (k.ghost())
                for (auto& t : T) ev[i].havoc.push_back(eval(k, sh, t));
            else
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
    // havoc assumption of loop M with its surviving candidates
    auto havoc_assumption = [&](std::size_t M) -> z3::expr {
        auto& CL = e.cut_loops[M];
        z3::expr_vector v(c);
        for (std::size_t i = 0; i < cands.size(); ++i)
            if (cands[i].alive && static_cast<std::size_t>(cands[i].loop) == M)
                for (auto& h : ev[i].havoc) v.push_back(h);
        if (v.empty()) return c.bool_val(true);
        return z3::implies(e.reach[static_cast<std::size_t>(CL.node)] && noviol(CL.props_before), z3::mk_and(v));
    };
    z3::solver s(c);
    add_all(s, e.assumptions);
    auto check_goal = [&](const std::function<bool(std::size_t)>& before, const z3::expr& at,
                          const std::function<std::optional<z3::expr>(std::size_t)>& goal_of, int L) -> bool {
        // returns true when something was dropped
        std::vector<std::size_t> who;
        for (std::size_t i = 0; i < cands.size(); ++i) {
            if (!cands[i].alive || cands[i].loop != L) continue;
            if (!goal_of(i)) continue;
            who.push_back(i);
        }
        if (who.empty()) return false;
        s.push();
        for (std::size_t M = 0; M < e.cut_loops.size(); ++M)
            if (e.cut_loops[M].node >= 0 && before(M)) s.add(havoc_assumption(M));
        s.add(at);
        z3::params p(c);
        p.set("timeout", static_cast<unsigned>(std::max(1.0, std::min(per_query_, left())) * 1000));
        s.set(p);
        // Drop every goal a model falsifies and ask again for the rest, in
        // this context (the context may still assume candidates dropped here:
        // a model of a stronger context is a counterexample for the weaker
        // one too, so a goal it falsifies is rightly dropped; the outer
        // rounds re-check the survivors with the final context).
        bool dropped = false;
        for (;;) {
            z3::expr_vector rest(c);
            std::vector<std::size_t> live;
            for (auto i : who)
                if (cands[i].alive) {
                    rest.push_back(!*goal_of(i));
                    live.push_back(i);
                }
            if (live.empty()) break;
            if (std::getenv("PRISM_HOUDINI_DEBUG"))
                std::fprintf(stderr, "houdini L%d goals=%zu t=%.1f\n", L, live.size(), deadline_ - left());
            z3::expr act = c.bool_const(("houdini!q" + std::to_string(query_++)).c_str());
            s.add(z3::implies(act, z3::mk_or(rest)));
            z3::expr_vector as(c);
            as.push_back(act);
            auto r = s.check(as);
            if (r == z3::unsat) break;
            if (r == z3::sat) {
                auto m = s.get_model();
                bool any = false;
                for (auto i : live)
                    if (!m.eval(*goal_of(i), true).is_true()) {
                        cands[i].alive = false;
                        any = true;
                    }
                if (!any)  // model evaluation disagrees: drop all (never keep an unproved one)
                    for (auto i : live) cands[i].alive = false;
                dropped = true;
                if (left() <= 0) break;
                continue;
            }
            for (auto i : live) cands[i].alive = false;  // no answer: nothing unproved is kept
            dropped = true;
            break;
        }
        s.pop();
        return dropped;
    };
    bool changed = true;
    while (changed) {
        if (left() <= 0) {
            out.why = "Houdini ran out of time";
            return out;
        }
        ++out.rounds;
        changed = false;
        for (std::size_t L = 0; L < e.cut_loops.size(); ++L) {
            auto& CL = e.cut_loops[L];
            if (CL.node < 0) continue;
            const int seqL = CL.seq;
            // base: the entry state
            changed |= check_goal([&](std::size_t M) { return e.cut_loops[M].seq < seqL; },
                                  e.reach[static_cast<std::size_t>(CL.node)] && noviol(CL.props_before),
                                  [&](std::size_t i) { return ev[i].entry; }, static_cast<int>(L));
            // step: the next state of every back edge
            for (std::size_t j = 0; j < CL.latches.size(); ++j) {
                auto& lt = CL.latches[j];
                const int seqT = lt.seq;
                changed |= check_goal([&](std::size_t M) { return e.cut_loops[M].seq <= seqT; },
                                      lt.guard && noviol(lt.props_before),
                                      [&](std::size_t i) -> std::optional<z3::expr> { return ev[i].next[j]; },
                                      static_cast<int>(L));
            }
        }
    }
    for (std::size_t i = 0; i < cands.size(); ++i)
        if (cands[i].alive) {
            out.invariants[static_cast<std::size_t>(cands[i].loop)].push_back(text(cands[i]));
            out.kept[static_cast<std::size_t>(cands[i].loop)].push_back(i);
        }
    // the cut program with every surviving invariant assumed
    s.push();
    for (std::size_t M = 0; M < e.cut_loops.size(); ++M)
        if (e.cut_loops[M].node >= 0) s.add(havoc_assumption(M));
    std::vector<z3::expr> viols;
    for (auto& p : e.props) viols.push_back(p.viol);
    s.add(any_of(c, viols));
    z3::params p(c);
    p.set("timeout", static_cast<unsigned>(std::max(per_query_, left()) * 1000));
    if (std::getenv("PRISM_HOUDINI_DEBUG")) std::fprintf(stderr, "houdini final t=%.1f\n", deadline_ - left());
    s.set(p);
    auto r = s.check();
    s.pop();
    if (r == z3::unsat) {
        out.proved = true;
        return out;
    }
    out.why = r == z3::sat ? "a property is not implied by the inductive invariants found"
                           : "cut program: solver " + std::string(r == z3::unknown ? "unknown" : "?");
    return out;
}

// Source-level export of the surviving invariants (SV-COMP correctness
// witnesses): filled in by the debug-information mapping; until then only
// the PIR text is reported.
void export_invariants(const Function&, const CfgInfo&, const HoudiniOut&, Verdict&) {}
