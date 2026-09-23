// Control-flow lowering for the Clang -> PIR translator (roadmap 2.3 and 2.6;
// docs/PIR.md "Exceptions", "setjmp/longjmp", "Indirect calls", "Inline
// assembly"). Included by translate.cpp inside its anonymous namespace,
// after struct Tr: these are Tr member functions.
//
// Exceptions. Every call is inlined, so the chain of enclosing invokes of a
// throw is known while translating it (Tr::handlers). A throw becomes an
// explicit edge: the exception object gets a 16-byte PRISM header (type id,
// "rethrown" flag) in front of the thrown object, and the throw jumps to the
// innermost landing pad whose clauses catch its type (exact typeinfo, public
// base classes from the module's typeinfo, the libstdc++ exception
// hierarchy, catch (...)) or that has cleanup code; the landingpad's
// { exn, selector } are phis over those edges. `resume` re-throws with the
// type read back from the header. Landing pads are translated after the
// normal blocks of their function and only when a throw reaches them; a
// landing pad whose code cannot be translated, or a type match PRISM cannot
// decide, ends the path with a *soft* check (NEEDS-HARNESS when reachable,
// never FAILED, never proved away). Properties: an exception reaching a
// noexcept boundary (__clang_call_terminate) is CXX-THROW-NOEXCEPT, one that
// leaves main is CXX-UNCAUGHT, std::terminate() is CXX-TERMINATE.
//
// setjmp/longjmp. A setjmp call splits its block: the continuation block's
// phi is the return value (0 on the direct path). setjmp stores its site id
// in the jmp_buf; longjmp reads it (memory checks apply: an unset jmp_buf is
// an uninitialised read) and jumps to the continuation of the site with that
// id whose frame is still active, returning v (1 when v == 0). A site whose
// function has returned is C11 7.13.2.1p2 UB: CTRL-LONGJMP-INVALID.
//
// Indirect calls. A function's address is a constant pointer whose object id
// lies in [kFnObjBase, kMaxObjects) (never allocated, never dereferenceable).
// A call through a pointer dispatches over the candidate targets with the
// call's signature: the functions stored in the module's vtables when the
// pointer is loaded from a vtable slot (class hierarchy analysis), otherwise
// every address-taken function. A null pointer is PTR-NULL-DEREF; any other
// value is the havoc fallback (soft check: NEEDS-HARNESS when reachable).
//
// Inline assembly. NEEDS-HARNESS unless the source gives a contract on the
// line(s) above it: `// prism: asm ensures <cond>` (cond: comparisons of the
// asm's output with integer constants joined by &&, or `true`). The block is
// then assumed to write only its output, satisfying cond; the assumption is
// listed and a proof is PROVED-ASSUMING.

struct Tr::Snap {
    std::size_t nblocks = 0, npphis = 0, nedges = 0, nlpad = 0, nsj = 0, ninlined = 0, nthrows = 0, nlibm = 0,
                nstack = 0, nmodel = 0, nhandlers = 0, nframes = 0, nassume = 0;
    std::vector<std::size_t> pphi_in;
    std::vector<std::tuple<std::size_t, std::size_t, Term>> blocks;  // stmts, phis, terminator
    std::map<std::string, int> head, tail;
};

Tr::Snap Tr::snap() {
    Snap s;
    s.nblocks = out.blocks.size();
    s.npphis = pphis.size();
    s.nedges = eh_edges.size();
    s.nlpad = lpad_phis.size();
    s.nsj = sj_sites.size();
    s.ninlined = out.inlined.size();
    s.nthrows = out.throws.size();
    s.nlibm = out.libm.size();
    s.nstack = stack.size();
    s.nmodel = model_stack.size();
    s.nhandlers = handlers.size();
    s.nframes = frames.size();
    s.nassume = out.assumptions.size();
    for (auto& p : pphis) s.pphi_in.push_back(p.in.size());
    for (auto& b : out.blocks) s.blocks.emplace_back(b.stmts.size(), b.phis.size(), b.term);
    s.head = head;
    s.tail = tail;
    return s;
}

// Undo everything translated since `s` (a region that turned out UNENCODED).
// Variables and prologue statements stay: they are unused afterwards.
void Tr::restore(const Snap& s) {
    out.blocks.resize(s.nblocks);
    for (std::size_t i = 0; i < s.nblocks; ++i) {
        auto& [ns, np, t] = s.blocks[i];
        out.blocks[i].stmts.resize(ns);
        out.blocks[i].phis.resize(np);
        out.blocks[i].term = t;
    }
    pphis.resize(s.npphis);
    for (std::size_t i = 0; i < s.npphis; ++i) pphis[i].in.resize(s.pphi_in[i]);
    eh_edges.resize(s.nedges);
    lpad_phis.resize(s.nlpad);
    sj_sites.resize(s.nsj);
    out.inlined.resize(s.ninlined);
    out.throws.resize(s.nthrows);
    out.libm.resize(s.nlibm);
    stack.resize(s.nstack);
    model_stack.resize(s.nmodel);
    handlers.resize(s.nhandlers);
    frames.resize(s.nframes);
    out.assumptions.resize(s.nassume);
    head = s.head;
    tail = s.tail;
}

// A path PRISM cannot follow: reachable means NEEDS-HARNESS (encode.cpp
// "soft" checks), never FAILED and never proved away.
void Tr::soft_stop(int b, const std::string& msg, int line) {
    Stmt s;
    s.kind = Stmt::Check;
    s.args = {Arg::c(1, 1)};
    s.prop = "unmodelled";
    s.cls = "UNENCODED";
    s.msg = msg;
    s.line = line;
    stmts(b).push_back(std::move(s));
    out.blocks[static_cast<std::size_t>(b)].term = Term{};
}

void Tr::add_phi(int block, int dst, int pred, Arg v) {
    for (auto& p : pphis)
        if (p.block == block && p.dst == dst) {
            p.in.emplace_back("", pred, v, 0);
            return;
        }
    PPhi p;
    p.block = block;
    p.dst = dst;
    p.in.emplace_back("", pred, v, 0);
    pphis.push_back(std::move(p));
}

Arg Tr::fn_addr(const std::string& name) {
    auto it = fn_ids.find(name);
    if (it == fn_ids.end()) {
        uint64_t id = kFnObjBase + fn_ids.size();
        if (id >= kMaxObjects) throw Unenc{"UNENCODED: more than " + std::to_string(kMaxObjects - kFnObjBase) +
                                           " function addresses"};
        it = fn_ids.emplace(name, id).first;
    }
    return Arg::c(kPtrW, it->second << kObjShift);
}

// ---- memory helpers (checked accesses through MemTr) ------------------------

Arg Tr::ld(int b, Arg p, unsigned w, int line) {
    int v = newvar(tmpname("ld"), w);
    ir::Type ty = w == kPtrW ? ir::Type{ir::Type::Ptr, 0, "ptr", {}} : ir::Type{ir::Type::Int, w, "i" + std::to_string(w), {}};
    mt.load(b, v, ty, p, 8, line);
    return Arg::v(v, w);
}

void Tr::st(int b, Arg p, Arg v, int line) {
    ir::Type ty = v.width == kPtrW && !v.is_const ? ir::Type{ir::Type::Int, 64, "i64", {}}
                                                  : ir::Type{ir::Type::Int, v.width, "i" + std::to_string(v.width), {}};
    mt.store(b, p, v, Arg::c(1, 1), ty, 8, line);
}

Arg Tr::at(int b, Arg p, int64_t off, int line) {
    int v = newvar(tmpname("p"), kPtrW);
    mt.gep(b, v, p, ir::Type{ir::Type::Int, 8, "i8", {}}, {Arg::c(64, static_cast<uint64_t>(off))}, true, line, 3);
    return Arg::v(v, kPtrW);
}

// ---- exceptions ---------------------------------------------------------------

namespace {

constexpr uint64_t kCatchAllSel = 0x7FFFFFF0;  // selector of catch (...) (never a type id)

// libstdc++ exception classes (typeinfo symbol -> public base), for throws
// by std::__throw_* and catch clauses naming them
const std::map<std::string, std::string>& std_exception_bases() {
    static const std::map<std::string, std::string> k{
        {"_ZTISt9exception", ""},
        {"_ZTISt9bad_alloc", "_ZTISt9exception"},
        {"_ZTISt20bad_array_new_length", "_ZTISt9bad_alloc"},
        {"_ZTISt11logic_error", "_ZTISt9exception"},
        {"_ZTISt12domain_error", "_ZTISt11logic_error"},
        {"_ZTISt16invalid_argument", "_ZTISt11logic_error"},
        {"_ZTISt12length_error", "_ZTISt11logic_error"},
        {"_ZTISt12out_of_range", "_ZTISt11logic_error"},
        {"_ZTISt13runtime_error", "_ZTISt9exception"},
        {"_ZTISt11range_error", "_ZTISt13runtime_error"},
        {"_ZTISt14overflow_error", "_ZTISt13runtime_error"},
        {"_ZTISt15underflow_error", "_ZTISt13runtime_error"},
        {"_ZTISt8bad_cast", "_ZTISt9exception"},
        {"_ZTISt10bad_typeid", "_ZTISt9exception"},
        {"_ZTISt19bad_optional_access", "_ZTISt9exception"},
        {"_ZTISt17bad_function_call", "_ZTISt9exception"},
        {"_ZTISt18bad_variant_access", "_ZTISt9exception"},
    };
    return k;
}

// std::__throw_* -> the typeinfo of what it throws (empty: unknown)
std::string std_throw_type(const std::string& fn) {
    static const std::vector<std::pair<std::string, std::string>> k{
        {"__throw_out_of_range_fmt", "_ZTISt12out_of_range"},
        {"__throw_out_of_range", "_ZTISt12out_of_range"},
        {"__throw_length_error", "_ZTISt12length_error"},
        {"__throw_bad_alloc", "_ZTISt9bad_alloc"},
        {"__throw_bad_array_new_length", "_ZTISt20bad_array_new_length"},
        {"__throw_logic_error", "_ZTISt11logic_error"},
        {"__throw_invalid_argument", "_ZTISt16invalid_argument"},
        {"__throw_domain_error", "_ZTISt12domain_error"},
        {"__throw_runtime_error", "_ZTISt13runtime_error"},
        {"__throw_range_error", "_ZTISt11range_error"},
        {"__throw_overflow_error", "_ZTISt14overflow_error"},
        {"__throw_underflow_error", "_ZTISt15underflow_error"},
        {"__throw_bad_optional_access", "_ZTISt19bad_optional_access"},
        {"__throw_bad_function_call", "_ZTISt17bad_function_call"},
        {"__throw_bad_variant_access", "_ZTISt18bad_variant_access"},
        {"__throw_bad_cast", "_ZTISt8bad_cast"},
        {"__throw_bad_typeid", "_ZTISt10bad_typeid"},
    };
    for (auto& [pre, ti] : k) {
        auto p = fn.find(pre);
        if (p == std::string::npos) continue;
        auto e = p + pre.size();
        // exact function name: the next char starts the parameter mangling
        if (e < fn.size() && (std::islower(static_cast<unsigned char>(fn[e])) || fn[e] == '_') && fn.compare(e, 3, "PKc") != 0 &&
            fn[e] != 'v' && fn[e] != 'b' && fn[e] != 'i')
            continue;
        return ti;
    }
    return {};
}

bool is_std_throw(const std::string& n) { return n.starts_with("_ZSt") && n.find("__throw_") != std::string::npos; }

bool fundamental_ti(const std::string& t) {
    static const std::set<std::string> k{"v", "b", "c", "a", "h", "s", "t", "i", "j", "l", "m", "x", "y",
                                         "n", "o", "f", "d", "e", "g", "w", "Ds", "Di", "Du", "Dn", "DF16_"};
    if (!t.starts_with("_ZTI")) return false;
    auto r = t.substr(4);
    if (r.starts_with("K")) r = r.substr(1);
    return k.count(r) > 0;
}

bool pointer_ti(const std::string& t) { return t.starts_with("_ZTIP"); }

}  // namespace

const ir::Inst* Tr::landingpad_of(const ir::Function& f, const std::string& lpad) {
    for (auto& b : f.blocks) {
        if (b.name != lpad) continue;
        for (auto& in : b.insts) {
            if (in.op == "phi") continue;
            return in.op == "landingpad" ? &in : nullptr;
        }
        return nullptr;
    }
    return nullptr;
}

uint64_t Tr::type_id(const std::string& tinfo) {
    auto it = type_ids.find(tinfo);
    if (it == type_ids.end()) it = type_ids.emplace(tinfo, type_ids.size() + 1).first;
    return it->second;
}

// Every exception type the module throws (typeinfo symbols; std::__throw_*
// types; an unknown library throw is named by its function), and the
// destructor __cxa_throw passes for each.
const std::vector<std::string>& Tr::thrown_types() {
    if (thrown_) return *thrown_;
    std::set<std::string> ts;
    for (auto& f : m.functions)
        for (auto& b : f.blocks)
            for (auto& in : b.insts) {
                if (in.op != "call" && in.op != "invoke") continue;
                if (in.callee == "__cxa_throw" && in.ops.size() >= 2 && in.ops[1].v.kind == ir::Value::Global) {
                    ts.insert(in.ops[1].v.name);
                    std::string d = in.ops.size() >= 3 && in.ops[2].v.kind == ir::Value::Global ? in.ops[2].v.name : "";
                    type_dtor.emplace(in.ops[1].v.name, d);
                } else if (is_std_throw(in.callee)) {
                    auto t = std_throw_type(in.callee);
                    ts.insert(t.empty() ? in.callee : t);
                }
            }
    thrown_ = std::vector<std::string>(ts.begin(), ts.end());
    return *thrown_;
}

// Does class typeinfo t have c as a public, unambiguous base (or equal)?
// 1 yes, 0 no, -1 unknown (hierarchy not visible).
int Tr::derives(const std::string& t, const std::string& c, int depth) {
    if (t == c) return 1;
    if (depth > 32) return -1;
    if (auto it = std_exception_bases().find(t); it != std_exception_bases().end())
        return it->second.empty() ? 0 : derives(it->second, c, depth + 1);
    const auto* g = m.find_global(t);
    if (!g || g->external || g->init.empty() || g->init[0].v.kind != ir::Value::Aggregate) return -1;
    const auto& el = g->init[0].v.elems;
    if (el.empty()) return -1;
    const auto& vt = el[0].v;
    std::string vtn = vt.kind == ir::Value::Global ? vt.name
                      : vt.kind == ir::Value::ConstExpr && !vt.elems.empty() && vt.elems[0].v.kind == ir::Value::Global
                          ? vt.elems[0].v.name
                          : "";
    if (vtn.find("__class_type_info") != std::string::npos) return 0;  // no bases
    if (vtn.find("__si_class_type_info") != std::string::npos) {
        if (el.size() < 3 || el[2].v.kind != ir::Value::Global) return -1;
        return derives(el[2].v.name, c, depth + 1);
    }
    if (vtn.find("__vmi_class_type_info") != std::string::npos) {
        // { vptr, name, flags, count, (base, offset_flags)* }; bit 1 of offset_flags: public
        int found = 0;
        bool unknown = false;
        for (std::size_t k = 4; k + 1 < el.size(); k += 2) {
            if (el[k].v.kind != ir::Value::Global || el[k + 1].v.kind != ir::Value::Int) return -1;
            if (!(el[k + 1].v.bits & 2)) continue;  // non-public base: no match through it
            int r = derives(el[k].v.name, c, depth + 1);
            if (r < 0) unknown = true;
            if (r > 0) ++found;
        }
        if (found > 1) return -1;  // ambiguous base: not modelled
        if (found == 1) return unknown ? -1 : 1;
        return unknown ? -1 : 0;
    }
    return -1;
}

int Tr::catches(const std::string& thrown, const ir::Operand& clause) {
    if (clause.v.kind == ir::Value::Null) return 1;  // catch (...)
    if (clause.v.kind != ir::Value::Global) return -1;
    const auto& c = clause.v.name;
    if (c == thrown) return 1;
    if (pointer_ti(c) || pointer_ti(thrown)) {
        if (pointer_ti(c) && pointer_ti(thrown)) return -1;  // qualification / derived-to-base conversions
        if (pointer_ti(c) && thrown == "_ZTIDn") return -1;  // throw nullptr
        return 0;
    }
    if (fundamental_ti(c) || fundamental_ti(thrown)) return 0;
    if (!thrown.starts_with("_ZTI")) return -1;  // unknown library throw
    return derives(thrown, c, 0);
}

// (1 enters with selector, 0 passes by, -1 unknown)
std::pair<int, uint64_t> Tr::enters(const ir::Inst& lp, const std::string& t) {
    for (auto& [kind, o] : lp.clauses) {
        if (kind != "catch") return {-1, 0};  // filter (dynamic exception specification)
        int r = catches(t, o);
        if (r < 0) return {-1, 0};
        if (r > 0) return {1, o.v.kind == ir::Value::Null ? kCatchAllSel : type_id(o.v.name)};
    }
    bool cleanup = std::find(lp.flags.begin(), lp.flags.end(), "cleanup") != lp.flags.end();
    return {cleanup ? 1 : 0, 0};
}

// Route an exception of a known type from block `cur` (whose terminator this sets).
void Tr::dispatch_type(int cur, Arg exn, const std::string& t, int line) {
    auto shown = t.starts_with("_ZTI") ? t.substr(4) : t;
    for (std::size_t i = handlers.size(); i-- > 0;) {
        auto& h = handlers[i];
        const auto* lp = landingpad_of(*h.fr->f, h.lpad);
        if (!lp) {
            soft_stop(cur, "exception path reachable: landing pad " + h.lpad + " not recognised", line);
            return;
        }
        auto [r, sel] = enters(*lp, t);
        if (r < 0) {
            soft_stop(cur, "exception path reachable: whether a handler catches " + shown + " is not modelled", line);
            return;
        }
        if (r == 0) continue;
        // frames between the throw and the handler are unwound: their locals end
        for (std::size_t k = h.depth; k < frames.size(); ++k)
            for (auto& a : frames[k]->allocas) mt.end_lifetime(cur, a);
        int lb = head.at(h.fr->prefix + h.lpad);
        Term j;
        j.kind = Term::Jmp;
        j.t = lb;
        out.blocks[static_cast<std::size_t>(cur)].term = j;
        eh_edges.push_back(EhEdge{h.fr->serial, h.lpad, h.invoke_block, cur});
        if (!lp->result.empty())
            if (auto it = h.fr->pairs.find(lp->result); it != h.fr->pairs.end()) {
                add_phi(lb, it->second.first, cur, exn);
                add_phi(lb, it->second.second, cur, Arg::c(32, sel));
            }
        return;
    }
    // no handler in the analysed function: the exception leaves it
    // main, or the dynamic initialisation of globals before it (a throwing
    // constructor of a global object): std::terminate
    const bool entry = !stack.empty() && (stack.front() == "main" || stack.front().starts_with("_GLOBAL__sub_I") ||
                                          stack.front().starts_with("__cxx_global_var_init"));
    if (entry)
        check(cur, Arg::c(1, 1), "uncaught", "CXX-UNCAUGHT",
              "exception of type " + shown + " escapes " +
                  (stack.front() == "main" ? std::string("main") : "static initialisation (" + stack.front() + ")") +
                  ": std::terminate is called",
              line);
    out.throws.push_back(shown);
    out.blocks[static_cast<std::size_t>(cur)].term = Term{};
}

// Throw `exn` (pointer to the PRISM header of the exception object) from
// `cur`; `type` known statically or read from the header. Leaves `cur` on a
// fresh unreachable block (the throw does not return).
void Tr::raise(int& cur, Arg exn, const std::optional<std::string>& type, int line) {
    if (type) {
        dispatch_type(cur, exn, *type, line);
    } else {
        Arg tid = ld(cur, exn, 64, line);
        for (auto& t : thrown_types()) {
            int yes = newblock("eh.type"), no = newblock("eh.next");
            Term br;
            br.kind = Term::Br;
            br.cond = assign(cur, Op::Eq, 1, {tid, Arg::c(64, type_id(t))}, "c");
            br.t = yes;
            br.f = no;
            out.blocks[static_cast<std::size_t>(cur)].term = br;
            dispatch_type(yes, exn, t, line);
            cur = no;
        }
        soft_stop(cur, "exception path reachable: exception of a type PRISM cannot identify", line);
    }
    cur = newblock("eh.after.throw");  // unreachable
}

Arg Tr::caught_slot() {
    // hidden object: [0] depth, [8 + 8k] the k-th caught exception (header pointer)
    if (!caught_) caught_ = mt.alloc(-1, Arg::c(64, 72), MemKind::Static, 1, "caught exceptions (PRISM)", 0);
    return *caught_;
}

bool Tr::eh_call(Frame& fr, const ir::Inst& in, int& cur, int line, bool& noreturn) {
    const auto& n = in.callee;
    auto arg = [&](std::size_t i) {
        if (i >= in.ops.size()) throw Unenc{"UNENCODED: call @" + n + " arity"};
        Arg a = operand(fr, in.ops[i], cur, line);
        return a;
    };
    auto set_result = [&](Arg v) {
        if (int dst = in.result.empty() ? -1 : result_var(fr, in); dst >= 0) emit_assign(cur, dst, Op::Copy, {v});
    };
    auto soft_guard = [&](Arg ok, const std::string& msg) {
        int yes = newblock("eh.ok"), no = newblock("eh.unmodelled");
        Term br;
        br.kind = Term::Br;
        br.cond = ok;
        br.t = yes;
        br.f = no;
        out.blocks[static_cast<std::size_t>(cur)].term = br;
        soft_stop(no, msg, line);
        cur = yes;
    };
    if (n == "__cxa_allocate_exception") {
        Arg sz = arg(0);
        if (sz.width < 64) sz = assign(cur, Op::ZExt, 64, {sz});
        Arg base = mt.alloc(cur, assign(cur, Op::Add, 64, {sz, Arg::c(64, 16)}), MemKind::Extern, 0, "exception object", line);
        st(cur, base, Arg::c(64, 0), line);                // type id (set by the throw)
        st(cur, at(cur, base, 8, line), Arg::c(64, 0), line);  // rethrown flag
        set_result(at(cur, base, 16, line));
        out.uses_memory = true;
        return true;
    }
    if (n == "__cxa_free_exception") {
        mt.end_lifetime(cur, at(cur, arg(0), -16, line));
        return true;
    }
    if (n == "__cxa_throw") {
        if (in.ops.size() < 2 || in.ops[1].v.kind != ir::Value::Global)
            throw Unenc{"UNENCODED: __cxa_throw with a non-constant type"};
        thrown_types();
        const auto& t = in.ops[1].v.name;
        Arg exn = at(cur, arg(0), -16, line);
        st(cur, exn, Arg::c(64, type_id(t)), line);
        raise(cur, exn, t, line);
        noreturn = true;
        return true;
    }
    if (is_std_throw(n) && !m.find(n)) {
        // std::__throw_*: a library exception object of the thrown class
        for (std::size_t i = 0; i < in.ops.size(); ++i)
            if (in.ops[i].ty.kind != ir::Type::Float) arg(i);
        thrown_types();
        auto t = std_throw_type(n);
        if (t.empty()) t = n;
        Arg base = mt.alloc(cur, Arg::c(64, 16 + 64), MemKind::Extern, 2, "exception object (" + n + ")", line);
        st(cur, base, Arg::c(64, type_id(t)), line);
        st(cur, at(cur, base, 8, line), Arg::c(64, 0), line);
        out.uses_memory = true;
        raise(cur, base, t, line);
        noreturn = true;
        return true;
    }
    if (n == "__cxa_begin_catch" || n == "__cxa_get_exception_ptr") {
        Arg exn = arg(0);
        if (n == "__cxa_begin_catch") {
            Arg slot = caught_slot();
            Arg d = ld(cur, slot, 64, line);
            soft_guard(assign(cur, Op::Ult, 1, {d, Arg::c(64, 8)}, "c"), "exception handlers nested deeper than 8");
            int e = newvar(tmpname("p"), kPtrW);
            mt.gep(cur, e, slot, ir::Type{ir::Type::Int, 64, "i64", {}}, {assign(cur, Op::Add, 64, {d, Arg::c(64, 1)})},
                   true, line, 2);
            st(cur, Arg::v(e, kPtrW), exn, line);
            st(cur, slot, assign(cur, Op::Add, 64, {d, Arg::c(64, 1)}), line);
            st(cur, at(cur, exn, 8, line), Arg::c(64, 0), line);  // not rethrown (yet)
        }
        set_result(at(cur, exn, 16, line));
        return true;
    }
    if (n == "__cxa_end_catch" || n == "__cxa_rethrow") {
        Arg slot = caught_slot();
        Arg d = ld(cur, slot, 64, line);
        if (n == "__cxa_rethrow")
            check(cur, assign(cur, Op::Eq, 1, {d, Arg::c(64, 0)}, "c"), "terminate", "CXX-TERMINATE",
                  "throw; without an exception being handled calls std::terminate", line);
        else
            soft_guard(assign(cur, Op::Ne, 1, {d, Arg::c(64, 0)}, "c"), "__cxa_end_catch without a caught exception");
        int e = newvar(tmpname("p"), kPtrW);
        mt.gep(cur, e, slot, ir::Type{ir::Type::Int, 64, "i64", {}}, {d}, true, line, 1);
        Arg exn = ld(cur, Arg::v(e, kPtrW), kPtrW, line);
        if (n == "__cxa_rethrow") {
            st(cur, at(cur, exn, 8, line), Arg::c(64, 1), line);
            raise(cur, exn, std::nullopt, line);
            noreturn = true;
            return true;
        }
        st(cur, slot, assign(cur, Op::Sub, 64, {d, Arg::c(64, 1)}), line);
        // not rethrown: destroy the exception object (its destructor, then the storage)
        Arg flag = ld(cur, at(cur, exn, 8, line), 64, line);
        int destroy = newblock("eh.destroy"), done = newblock("eh.done");
        Term br;
        br.kind = Term::Br;
        br.cond = assign(cur, Op::Eq, 1, {flag, Arg::c(64, 0)}, "c");
        br.t = destroy;
        br.f = done;
        out.blocks[static_cast<std::size_t>(cur)].term = br;
        cur = destroy;
        thrown_types();
        Arg tid = ld(cur, exn, 64, line);
        Arg payload = at(cur, exn, 16, line);
        for (auto& [t, dtor] : type_dtor) {
            if (dtor.empty()) continue;
            int yes = newblock("eh.dtor"), no = newblock("eh.dtor.next");
            Term b2;
            b2.kind = Term::Br;
            b2.cond = assign(cur, Op::Eq, 1, {tid, Arg::c(64, type_id(t))}, "c");
            b2.t = yes;
            b2.f = no;
            out.blocks[static_cast<std::size_t>(cur)].term = b2;
            int c = yes;
            auto s = snap();
            try {
                const std::string tmp = "__prism.eh.obj";
                fr.env[tmp] = payload;
                ir::Inst call;
                call.op = "call";
                call.callee = dtor;
                call.ty = ir::Type{ir::Type::Void, 0, "void", {}};
                ir::Operand o;
                o.ty = ir::Type{ir::Type::Ptr, 0, "ptr", {}};
                o.v.kind = ir::Value::Local;
                o.v.name = tmp;
                call.ops = {o};
                bool nr = false;
                this->call(fr, call, c, ir::DILoc{line, 0}, nr);
                fr.env.erase(tmp);
                Term j;
                if (!nr) {
                    j.kind = Term::Jmp;
                    j.t = no;
                }
                out.blocks[static_cast<std::size_t>(c)].term = j;
            } catch (const Unenc& u) {
                restore(s);
                fr.env.erase("__prism.eh.obj");
                soft_stop(yes, "exception object destructor @" + dtor + ": " + u.reason, line);
            }
            cur = no;
        }
        mt.end_lifetime(cur, exn);
        Term j;
        j.kind = Term::Jmp;
        j.t = done;
        out.blocks[static_cast<std::size_t>(cur)].term = j;
        cur = done;
        return true;
    }
    if (starts(n, "llvm.eh.typeid.for")) {
        if (in.ops.empty() || in.ops[0].v.kind != ir::Value::Global) throw Unenc{"UNENCODED: call @" + n};
        set_result(Arg::c(32, type_id(in.ops[0].v.name)));
        return true;
    }
    if (n == "__clang_call_terminate") {
        check(cur, Arg::c(1, 1), "terminate", "CXX-THROW-NOEXCEPT",
              "exception escapes a noexcept function (or a destructor during unwinding): std::terminate", line);
        noreturn = true;
        return true;
    }
    if (n == "_ZSt9terminatev") {
        check(cur, Arg::c(1, 1), "terminate", "CXX-TERMINATE", "std::terminate() is called (process abort)", line);
        noreturn = true;
        return true;
    }
    if (n == "__cxa_pure_virtual") {
        check(cur, Arg::c(1, 1), "pure-virtual", "CXX-PURE-VIRTUAL",
              "pure virtual function called (undefined behaviour)", line);
        noreturn = true;
        return true;
    }
    if (n == "__cxa_call_unexpected") throw Unenc{"UNENCODED: dynamic exception specification (__cxa_call_unexpected)"};
    return false;
}

// Landing pads of `fr` that a throw reaches, with the code after them.
void Tr::eh_pass(Frame& fr) {
    std::set<std::string> tried;
    while (true) {
        std::string next;
        for (auto& e : eh_edges)
            if (e.frame == fr.serial && !tail.count(fr.prefix + e.lpad) && !tried.count(e.lpad)) {
                next = e.lpad;
                break;
            }
        if (next.empty()) return;
        tried.insert(next);
        auto s = snap();
        try {
            region(fr, next);
        } catch (const Unenc& u) {
            restore(s);
            int hb = head.at(fr.prefix + next);
            soft_stop(hb, "exception path reachable: landing pad code not encoded (" + u.reason + ")", 0);
            tail[fr.prefix + next] = hb;
        }
    }
}

void Tr::region(Frame& fr, const std::string& start) {
    std::map<std::string, const ir::Block*> by;
    for (auto& b : fr.f->blocks) by[b.name] = &b;
    std::vector<std::string> work{start};
    while (!work.empty()) {
        auto nme = work.back();
        work.pop_back();
        auto it = by.find(nme);
        if (it == by.end() || tail.count(fr.prefix + nme)) continue;
        translate_block(fr, *it->second);
        for (auto& in : it->second->insts) {
            if (in.op == "br" || in.op == "switch")
                for (auto& t : in.targets) work.push_back(t);
            if (in.op == "switch")
                for (auto& [c, t] : in.cases) work.push_back(t);
            if (in.op == "invoke" && !in.targets.empty()) work.push_back(in.targets[0]);
        }
    }
}

// ---- setjmp / longjmp ------------------------------------------------------------

bool Tr::sjlj_call(Frame& fr, const ir::Inst& in, int& cur, int line, bool& noreturn) {
    const auto& n = in.callee;
    const bool is_set = n == "_setjmp" || n == "setjmp" || n == "__sigsetjmp" || n == "sigsetjmp";
    const bool is_long = n == "longjmp" || n == "_longjmp" || n == "siglongjmp" || n == "__longjmp_chk";
    if ((!is_set && !is_long) || m.find(n)) return false;
    if (in.ops.empty()) throw Unenc{"UNENCODED: call @" + n + " arity"};
    Arg buf = operand(fr, in.ops[0], cur, line);
    if (is_set) {
        // the continuation block and site id are fixed per call site of this frame
        int dst = in.result.empty() ? -1 : result_var(fr, in);
        SjSite site{fr.serial, static_cast<int>(sj_sites.size()) + 1, newblock("setjmp.cont"), dst};
        sj_sites.push_back(site);
        st(cur, buf, Arg::c(64, static_cast<uint64_t>(site.id)), line);
        Term j;
        j.kind = Term::Jmp;
        j.t = site.cont;
        out.blocks[static_cast<std::size_t>(cur)].term = j;
        if (dst >= 0) add_phi(site.cont, dst, cur, Arg::c(out.vars[static_cast<std::size_t>(dst)].width, 0));
        cur = site.cont;
        return true;
    }
    if (in.ops.size() < 2) throw Unenc{"UNENCODED: call @" + n + " arity"};
    Arg v = operand(fr, in.ops[1], cur, line);
    Arg v1 = assign(cur, Op::Select, v.width,
                    {assign(cur, Op::Eq, 1, {v, Arg::c(v.width, 0)}, "c"), Arg::c(v.width, 1), v}, "ret");
    Arg id = ld(cur, buf, 64, line);  // an unset jmp_buf is an uninitialised read
    for (auto& s : sj_sites) {
        std::size_t depth = 0;
        bool live = false;
        for (std::size_t k = 0; k < frames.size(); ++k)
            if (frames[k]->serial == s.frame) {
                live = true;
                depth = k + 1;
            }
        if (!live) continue;
        int yes = newblock("longjmp.to"), no = newblock("longjmp.next");
        Term br;
        br.kind = Term::Br;
        br.cond = assign(cur, Op::Eq, 1, {id, Arg::c(64, static_cast<uint64_t>(s.id))}, "c");
        br.t = yes;
        br.f = no;
        out.blocks[static_cast<std::size_t>(cur)].term = br;
        for (std::size_t k = depth; k < frames.size(); ++k)
            for (auto& a : frames[k]->allocas) mt.end_lifetime(yes, a);
        Term j;
        j.kind = Term::Jmp;
        j.t = s.cont;
        out.blocks[static_cast<std::size_t>(yes)].term = j;
        if (s.dst >= 0) {
            auto w = out.vars[static_cast<std::size_t>(s.dst)].width;
            Arg r = v1;
            if (r.width != w) r = assign(yes, r.width < w ? Op::SExt : Op::Trunc, w, {r}, "ret");
            add_phi(s.cont, s.dst, yes, r);
        }
        cur = no;
    }
    check(cur, Arg::c(1, 1), "longjmp", "CTRL-LONGJMP-INVALID",
          "longjmp to a jmp_buf whose setjmp caller is no longer active (C11 7.13.2.1p2)", line);
    out.blocks[static_cast<std::size_t>(cur)].term = Term{};
    cur = newblock("longjmp.after");
    noreturn = true;
    return true;
}

// ---- indirect calls (virtual dispatch, function pointers) ---------------------------

void Tr::indirect_call(Frame& fr, const ir::Inst& in, int& cur, ir::DILoc l, bool& noreturn) {
    int line = l.line;
    if (!in.callee_op) throw Unenc{"UNENCODED: indirect call"};
    Arg fp = operand(fr, *in.callee_op, cur, line);
    fp.width = kPtrW;
    check(cur, assign(cur, Op::Eq, 1, {fp, Arg::c(kPtrW, 0)}, "c"), "null-call", "PTR-NULL-DEREF",
          "call through a null function pointer", line);
    // Is the pointer loaded from a vtable slot? (load (gep ptr (load this), K) or load (load this))
    auto def = [&](const ir::Operand& o) -> const ir::Inst* {
        if (o.v.kind != ir::Value::Local) return nullptr;
        auto it = fr.defs.find(o.v.name);
        return it == fr.defs.end() ? nullptr : it->second;
    };
    bool vcall = false;
    if (const auto* ld1 = def(*in.callee_op); ld1 && ld1->op == "load" && !ld1->ops.empty()) {
        const auto* slot = def(ld1->ops[0]);
        const ir::Inst* vt = slot;
        if (slot && slot->op == "getelementptr" && !slot->ops.empty()) vt = def(slot->ops[0]);
        vcall = vt && vt->op == "load";
    }
    // candidate targets with the call's signature: the functions in the
    // module's vtables for a vtable-slot load, else (or when no vtable
    // function fits) every address-taken function. Any other pointer value
    // reaches the havoc fallback below, so the choice only affects
    // completeness, never soundness.
    auto collect = [&](bool vt) {
        std::set<std::string> pool;
        auto scan_value = [&](auto&& self, const ir::Value& v) -> void {
            if (v.kind == ir::Value::Global && (m.find(v.name) || std::find(m.declarations.begin(), m.declarations.end(),
                                                                             v.name) != m.declarations.end()))
                pool.insert(v.name);
            for (auto& e : v.elems) self(self, e.v);
        };
        for (auto& g : m.globals) {
            if (vt && !g.name.starts_with("_ZTV")) continue;
            for (auto& o : g.init) scan_value(scan_value, o.v);
        }
        if (!vt)
            for (auto& f : m.functions)
                for (auto& b : f.blocks)
                    for (auto& x : b.insts) {
                        for (auto& o : x.ops) scan_value(scan_value, o.v);
                        for (auto& [o, _] : x.incoming) scan_value(scan_value, o.v);
                    }
        std::vector<std::string> cands;
        for (auto& name : pool) {
            const auto* f = m.find(name);
            if (!f) {
                if (name == "__cxa_pure_virtual" || name == "__cxa_deleted_virtual") cands.push_back(name);
                else if (vt) cands.push_back(name);  // a virtual function defined elsewhere: its branch is soft
                continue;
            }
            if (!f->parse_error.empty() && f->parse_error.find("variadic") != std::string::npos) continue;
            if (f->params.size() != in.ops.size() || f->ret.text != in.ty.text) continue;
            bool ok = true;
            for (std::size_t i = 0; i < in.ops.size() && ok; ++i) ok = f->params[i].ty.text == in.ops[i].ty.text;
            if (ok) cands.push_back(name);
        }
        return cands;
    };
    auto cands = collect(vcall);
    bool real = std::any_of(cands.begin(), cands.end(), [&](const std::string& n) { return m.find(n) != nullptr; });
    if (vcall && !real) {
        vcall = false;
        cands = collect(false);
    }
    constexpr std::size_t kMaxTargets = 16;
    if (cands.size() > kMaxTargets)
        throw Unenc{"UNENCODED: indirect call with " + std::to_string(cands.size()) + " possible targets (> " +
                    std::to_string(kMaxTargets) + ")"};
    out.uses_memory = true;  // function addresses are pointers of the memory model
    int dst = in.result.empty() ? -1 : result_var(fr, in);
    unsigned w = dst >= 0 ? out.vars[static_cast<std::size_t>(dst)].width : 0;
    int join = newblock("icall.join");
    bool any = false;
    for (auto& name : cands) {
        int yes = newblock("icall." + name), no = newblock("icall.next");
        Term br;
        br.kind = Term::Br;
        br.cond = assign(cur, Op::Eq, 1, {fp, fn_addr(name)}, "c");
        br.t = yes;
        br.f = no;
        out.blocks[static_cast<std::size_t>(cur)].term = br;
        int c = yes;
        auto s = snap();
        auto saved = in.result.empty() ? std::optional<Arg>{} : std::optional<Arg>{fr.env[in.result]};
        try {
            ir::Inst dc = in;
            dc.callee = name;
            dc.callee_op.reset();
            int rv = -1;
            if (dst >= 0) {
                rv = newvar(tmpname("icall"), w);
                out.vars[static_cast<std::size_t>(rv)].fp = out.vars[static_cast<std::size_t>(dst)].fp;
                fr.env[in.result] = Arg::v(rv, w);
            }
            bool nr = false;
            call(fr, dc, c, l, nr);
            if (saved) fr.env[in.result] = *saved;
            Term j;
            if (!nr) {
                j.kind = Term::Jmp;
                j.t = join;
                if (dst >= 0) add_phi(join, dst, c, Arg::v(rv, w));
                any = true;
            }
            out.blocks[static_cast<std::size_t>(c)].term = j;
        } catch (const Unenc& u) {
            restore(s);
            if (saved) fr.env[in.result] = *saved;
            soft_stop(yes, "indirect call to @" + name + " not encoded (" + u.reason + ")", line);
        }
        cur = no;
    }
    soft_stop(cur, "indirect call to a target outside the " + std::to_string(cands.size()) +
                       (vcall ? " virtual functions of the module's vtables" : " address-taken functions") +
                       " (havoc fallback)",
              line);
    cur = join;
    if (!any) noreturn = true;
}

// ---- inline assembly ----------------------------------------------------------------

void Tr::asm_call(Frame& fr, const ir::Inst& in, int& cur, int line) {
    (void)fr;
    auto it = opt.asm_contracts.find(line);
    if (it == opt.asm_contracts.end())
        throw Unenc{"UNENCODED: inline assembly (state its effect with `// prism: asm ensures <cond>` on the line above)"};
    for (auto& o : in.ops)
        if (o.ty.kind == ir::Type::Ptr)
            throw Unenc{"UNENCODED: inline assembly with memory operands (contract at line " + std::to_string(line) + ")"};
    int dst = in.result.empty() ? -1 : result_var(fr, in);
    if (in.ty.kind != ir::Type::Void && in.ty.kind != ir::Type::Int)
        throw Unenc{"UNENCODED: inline assembly with outputs of type " + in.ty.text};
    std::optional<Arg> outv;
    if (dst >= 0) outv = havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
    // cond := 'true' | cmp ('&&' cmp)* ; cmp := term op term ; term := identifier (the output) | integer
    const std::string& text = it->second;
    std::size_t p = 0;
    auto ws = [&] {
        while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    };
    auto term = [&]() -> Arg {
        ws();
        if (p < text.size() && (std::isalpha(static_cast<unsigned char>(text[p])) || text[p] == '_')) {
            while (p < text.size() && (std::isalnum(static_cast<unsigned char>(text[p])) || text[p] == '_')) ++p;
            if (!outv) throw Unenc{"UNENCODED: asm contract names an output but the asm has none (line " +
                                   std::to_string(line) + ")"};
            return *outv;
        }
        auto q = p;
        if (p < text.size() && text[p] == '-') ++p;
        while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) ++p;
        if (q == p || (text[q] == '-' && q + 1 == p))
            throw Unenc{"UNENCODED: asm contract not understood: " + text};
        long long v = std::stoll(text.substr(q, p - q));
        return Arg::c(outv ? outv->width : 64, static_cast<uint64_t>(v));
    };
    ws();
    if (text.compare(p, 4, "true") == 0) {
        p += 4;
    } else {
        while (true) {
            Arg a = term();
            ws();
            static const std::vector<std::pair<std::string, Op>> ops{{"==", Op::Eq}, {"!=", Op::Ne}, {"<=", Op::Sle},
                                                                     {">=", Op::Sge}, {"<", Op::Slt}, {">", Op::Sgt}};
            std::optional<Op> o;
            for (auto& [s, op] : ops)
                if (text.compare(p, s.size(), s) == 0) {
                    o = op;
                    p += s.size();
                    break;
                }
            if (!o) throw Unenc{"UNENCODED: asm contract not understood: " + text};
            Arg b = term();
            if (a.is_const && b.is_const) throw Unenc{"UNENCODED: asm contract not understood: " + text};
            a.width = b.width = a.is_const ? b.width : a.width;
            assume(cur, assign(cur, *o, 1, {a, b}, "c"));
            ws();
            if (text.compare(p, 2, "&&") == 0) {
                p += 2;
                continue;
            }
            break;
        }
    }
    ws();
    if (p != text.size()) throw Unenc{"UNENCODED: asm contract not understood: " + text};
    out.assumptions.push_back("inline assembly at line " + std::to_string(line) +
                              " writes only its output and ensures " + text + " (asm contract, not verified)");
}
