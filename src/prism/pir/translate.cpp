// LLVM IR (subset) -> PIR, with PRISM's own property instrumentation.
//
// Law 8 polarity: Clang is invoked without any -fsanitize check insertion;
// every property below is inserted here, so what is checked is decided by
// PRISM, never by a compiler flag.
//
//   add/sub/mul nsw        signed overflow            INT-SIGNED-OVF
//   sdiv/srem              divisor 0, INT_MIN / -1    INT-DIV-ZERO / INT-SIGNED-OVF
//   udiv/urem              divisor 0                  INT-DIV-ZERO
//   shl/lshr/ashr          amount >= width            INT-SHIFT-UB
//   shl (C signed <<)      negative base / overflow   INT-SHIFT-UB (located via clang)
//   nuw/exact/disjoint/nneg poison flags              UB-POISON
//   llvm.abs(x, true)      abs(INT_MIN)               INT-SIGNED-OVF
//   llvm.ctlz/cttz(x,true) zero input                 INT-CLZ-ZERO
//   unreachable / trap     reached                    CXX-UNREACHABLE
//   __assert_fail          reached                    FUNC-CONTRACT
//   read of an uninitialised local (instrumented before mem2reg) UNINIT-READ
//   clang-folded UB (poison constant / UB diagnostic)  per diagnostic
//   undef: a fresh value at every use; br on it, or passing/returning it
//          as noundef                                 UB-POISON (prop "undef")
//   memory (alloca/load/store/getelementptr/memcpy/free ...): translate_mem.cpp
//   floating point (IEEE, fptosi range FLOAT-CAST-OVF): translate_fp.cpp
//   exceptions, setjmp/longjmp, indirect calls, inline asm: translate_ctl.inc
//
// Everything else is thrown as "UNENCODED: <construct>" (roadmap 2.1).

#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include "fp.hpp"
#include "translate_fp.hpp"
#include "translate_mem.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace prism::pir {
namespace {

using pirmem::Unenc;
using pirmem::kPtrW;

bool has_flag(const ir::Inst& in, std::string_view f) {
    return std::find(in.flags.begin(), in.flags.end(), f) != in.flags.end();
}

unsigned int_width(const ir::Type& t, std::string_view what = "type") {
    if (t.kind != ir::Type::Int) throw Unenc{"UNENCODED: " + std::string(what) + " " + t.text};
    if (t.bits == 0 || t.bits > 64)
        throw Unenc{"UNENCODED: " + std::string(what) + " " + t.text + " (wider than 64 bits)"};
    return t.bits;
}

bool is_overflow_intrinsic(std::string_view n) {
    for (auto p : {"llvm.sadd.with.overflow.", "llvm.uadd.with.overflow.", "llvm.ssub.with.overflow.",
                   "llvm.usub.with.overflow.", "llvm.smul.with.overflow.", "llvm.umul.with.overflow."})
        if (n.starts_with(p)) return true;
    return false;
}

bool starts(std::string_view s, std::string_view p) { return s.starts_with(p); }

// C signedness of a __VERIFIER_nondet_<type>() result (the IR type has none):
// uint, ulong, uchar, ushort, unsigned, ulonglong, u32, ..., bool, _Bool,
// size_t, pointer are unsigned; char is signed (x86-64 and AArch64 Linux differ,
// SV-COMP's LP64 tasks follow x86-64).
bool nondet_is_unsigned(std::string_view fn) {
    auto t = fn.substr(std::string_view("__VERIFIER_nondet_").size());
    return t.starts_with("u") || t == "bool" || t == "_Bool" || t == "size_t" || t == "pointer";
}

struct Frame {
    const ir::Function* f = nullptr;
    std::string prefix;
    std::map<std::string, Arg> env;
    // two-field aggregate values kept as two variables: with.overflow
    // results, { ptr, i32 } exception pairs, and scalar pairs such as the
    // { ptr, i8 } an ABI returns std::pair<iterator, bool> in (scalar_pair)
    std::map<std::string, std::pair<int, int>> pairs;
    std::map<int, Arg> shadow;                          // var -> uninit shadow (i1)
    // var -> undef shadow (i1): the value may be (computed from) LLVM undef,
    // so every use may observe a different value (refinement finding 4)
    std::map<int, Arg> undef;
    std::map<std::string, std::array<bool, 2>> upair;   // pair fields that may be undef
    int ret_block = -1;
    int ret_var = -1;
    std::optional<std::pair<int, int>> ret_pair;        // inlined callee returning a scalar pair
    std::map<std::string, const ir::Inst*> defs;        // result name -> defining instruction
    std::vector<Arg> allocas;                           // entry-block stack objects (end at return)
    int site_line = 0;                                  // inside a library model: the call site's line
    std::set<int> raw;                                  // shadowed vars that are raw byte copies
    int ret_shadow = -1;                                // uninit shadow of the returned value (inlined)
    bool ret_shadow_used = false;
    int serial = 0;                                     // unique per inlined instance (translate_ctl.inc)
};

struct PPhi {
    int block = -1;
    int dst = -1;
    // (pred qualified name or "", pred block index when known, value, kind)
    // kind: 0 value, 1 undef, 2 poison
    std::vector<std::tuple<std::string, int, Arg, int>> in;
};

struct Tr final : pirmem::TrApi {
    const ir::Module& m;
    const TranslateOptions& opt;
    Function out;
    std::map<std::string, int> head, tail;
    std::vector<PPhi> pphis;
    std::vector<std::string> stack;
    std::set<int> folded_used;
    int uniq = 0;
    std::vector<Stmt> prologue;  // runs before block 0 (globals, pointer-parameter objects)
    std::vector<std::string> model_stack;  // library models being inlined (outermost first)
    std::string top_file;                  // source file of the analysed function

    // ---- control-flow lowering (translate_ctl.inc; docs/PIR.md "Exceptions",
    // "setjmp/longjmp", "Indirect calls", "Inline assembly") ----
    struct Handler {                       // an enclosing invoke: where an exception goes
        Frame* fr;
        std::string lpad;                  // landing pad block (IR name in fr)
        std::string invoke_block;          // block of the invoke (the IR predecessor of lpad)
        std::size_t depth;                 // frames[depth..] are unwound when it catches
    };
    struct EhEdge {                        // a throw that reaches a landing pad
        int frame;                         // Frame::serial
        std::string lpad, invoke_block;
        int pred;                          // PIR block that jumps to the landing pad
    };
    struct LpadPhi {                       // IR phi in a landing pad (incoming = invoke blocks)
        int frame;
        std::string lpad;
        int block, dst;
        std::vector<std::pair<std::string, Arg>> in;
    };
    struct SjSite {                        // setjmp call site (continuation block, result phi)
        int frame, id, cont, dst;
    };
    struct Snap;
    std::vector<Handler> handlers;
    std::vector<EhEdge> eh_edges;
    std::vector<LpadPhi> lpad_phis;
    std::vector<Frame*> frames;            // inlined frames, outermost first
    int frame_serial = 0;
    std::vector<SjSite> sj_sites;
    std::map<std::string, uint64_t> type_ids;       // typeinfo symbol -> selector value
    std::map<std::string, std::string> type_dtor;   // thrown typeinfo -> destructor symbol ("" none)
    std::map<std::string, uint64_t> fn_ids;         // function symbol -> object id of its address
    std::optional<std::vector<std::string>> thrown_;  // every type the module throws
    std::optional<Arg> caught_;                       // hidden object: stack of caught exceptions
    std::optional<Arg> oom_;                          // hidden object: an allocation failed (F9)
    bool oom_pending_ = false;                        // oom_ preassigned, its object not yet emitted

    Arg fn_addr(const std::string& name) override;
    Snap snap();
    void restore(const Snap& s);
    void soft_stop(int b, const std::string& msg, int line);
    void add_phi(int block, int dst, int pred, Arg v);
    void eh_pass(Frame& fr);
    void region(Frame& fr, const std::string& start);
    static const ir::Inst* landingpad_of(const ir::Function& f, const std::string& lpad);
    uint64_t type_id(const std::string& tinfo);
    const std::vector<std::string>& thrown_types();
    int derives(const std::string& t, const std::string& c, int depth, int64_t* off = nullptr);
    int catches(const std::string& thrown, const ir::Operand& clause, int64_t* off = nullptr);
    std::pair<int, uint64_t> enters(const ir::Inst& lp, const std::string& t, int64_t* bind = nullptr);
    void raise(int& cur, Arg exn, const std::optional<std::string>& type, int line);
    void dispatch_type(int cur, Arg exn, const std::string& type, int line);
    Arg caught_slot();
    Arg ld(int b, Arg p, unsigned w, int line);
    void st(int b, Arg p, Arg v, int line);
    Arg at(int b, Arg p, int64_t off, int line);
    bool eh_call(Frame& fr, const ir::Inst& in, int& cur, int line, bool& noreturn);
    bool sjlj_call(Frame& fr, const ir::Inst& in, int& cur, int line, bool& noreturn);
    void indirect_call(Frame& fr, const ir::Inst& in, int& cur, ir::DILoc l, bool& noreturn);
    void asm_call(Frame& fr, const ir::Inst& in, int& cur, int line);

    static std::string model_display(const std::string& n) {
        static const std::map<std::string, std::string> k{
            {"_Znwm", "operator new"},       {"_Znam", "operator new[]"},     {"_ZdlPv", "operator delete"},
            {"_ZdlPvm", "operator delete"}, {"_ZdaPv", "operator delete[]"}, {"_ZdaPvm", "operator delete[]"}};
        auto it = k.find(n);
        return it == k.end() ? n : it->second;
    }
    pirmem::MemTr mt;
    pirfp::FpTr fpt;

    Tr(const ir::Module& mm, const TranslateOptions& o) : m(mm), opt(o), mt(*this, mm), fpt(*this) {}

    // pirmem::TrApi
    const ir::Module& module() const override { return m; }
    const TranslateOptions& options() const override { return opt; }
    Function& fn() override { return out; }
    std::vector<Stmt>& stmts(int b) {
        return b < 0 ? prologue : out.blocks[static_cast<std::size_t>(b)].stmts;
    }
    void push(int b, Stmt s) override { stmts(b).push_back(std::move(s)); }

    ir::DILoc loc(const ir::Inst& in) const {
        if (in.dbg.empty()) return {};
        auto it = m.locs.find(in.dbg);
        return it == m.locs.end() ? ir::DILoc{} : it->second;
    }

    int newvar(const std::string& name, unsigned w) override {
        out.vars.push_back(Var{name, w});
        return static_cast<int>(out.vars.size()) - 1;
    }
    int newblock(const std::string& name) {
        out.blocks.push_back(Block{name, {}, {}, {}});
        return static_cast<int>(out.blocks.size()) - 1;
    }
    std::string tmpname(const char* base) { return std::string("_") + base + std::to_string(uniq++); }

    void emit_assign(int b, int dst, Op op, std::vector<Arg> args) {
        Stmt s;
        s.kind = Stmt::Assign;
        s.dst = dst;
        s.op = op;
        s.args = std::move(args);
        stmts(b).push_back(std::move(s));
    }
    Arg assign(int b, Op op, unsigned w, std::vector<Arg> args, const char* base = "t") override {
        int v = newvar(tmpname(base), w);
        emit_assign(b, v, op, std::move(args));
        return Arg::v(v, w);
    }
    void check(int b, Arg viol, std::string prop, std::string cls, std::string msg, int line) override {
        if (viol.is_const && viol.bits == 0) return;  // statically safe
        Stmt s;
        s.kind = Stmt::Check;
        s.args = {viol};
        s.prop = std::move(prop);
        s.cls = std::move(cls);
        s.msg = std::move(msg);
        s.line = line;
        if (!model_stack.empty()) s.msg += " (in the " + model_stack.front() + " library model)";
        stmts(b).push_back(std::move(s));
    }
    void assume(int b, Arg cond) override {
        Stmt s;
        s.kind = Stmt::Assume;
        s.args = {cond};
        stmts(b).push_back(std::move(s));
    }
    Arg havoc(int b, unsigned w, bool uninit, bool nondet, int dst = -1) override {
        if (dst < 0) dst = newvar(tmpname(uninit ? "uninit" : "undef"), w);
        Stmt s;
        s.kind = Stmt::Assign;
        s.dst = dst;
        s.op = Op::Havoc;
        s.uninit = uninit;
        s.nondet = nondet;
        stmts(b).push_back(std::move(s));
        return Arg::v(dst, w);
    }

    // iN (<= 64) or ptr
    unsigned vwidth(const ir::Type& t, std::string_view what = "type") const { return mt.value_width(t, what); }

    // A first-class struct of two scalar fields (iN <= 64 or ptr), e.g. the
    // { ptr, i8 } a std::pair<iterator, bool> is returned in: kept as two
    // variables (Frame::pairs). Returns the field widths.
    static std::optional<std::pair<unsigned, unsigned>> scalar_pair(const ir::Type& t) {
        if (t.kind != ir::Type::Struct || t.elems.size() != 2) return std::nullopt;
        auto w = [](const ir::Type& e) -> unsigned {
            if (e.kind == ir::Type::Ptr && e.text == "ptr") return kPtrW;
            if (e.kind == ir::Type::Int && e.bits > 0 && e.bits <= 64) return e.bits;
            return 0;
        };
        unsigned a = w(t.elems[0]), b = w(t.elems[1]);
        if (!a || !b) return std::nullopt;
        return std::pair{a, b};
    }

    int var_of(Frame& fr, const std::string& name) {
        auto it = fr.env.find(name);
        if (it == fr.env.end() || it->second.is_const) return -1;
        return it->second.var;
    }

    // A non-phi use of an operand. Reads of a maybe-uninitialised local are
    // checked here (C11 6.3.2.1p2: automatic object whose address is never
    // taken, read while indeterminate).
    Arg operand(Frame& fr, const ir::Operand& o, int b, int line, bool use = true) {
        switch (o.v.kind) {
            case ir::Value::Int: return Arg::c(int_width(o.ty, "operand type"), o.v.bits);
            case ir::Value::Local: {
                if (fr.pairs.count(o.v.name)) throw Unenc{"UNENCODED: aggregate value %" + o.v.name};
                auto it = fr.env.find(o.v.name);
                if (it == fr.env.end())
                    throw Unenc{"UNENCODED: value %" + o.v.name + " of type " + o.ty.text};
                Arg a = it->second;
                if (use && !a.is_const) {
                    if (auto sh = fr.shadow.find(a.var); sh != fr.shadow.end()) {
                        Arg s = sh->second;
                        bool bytes = s.width > 1;  // raw byte copy: per-byte mask
                        if (bytes) s = assign(b, Op::Ne, 1, {s, Arg::c(s.width, 0)}, "u");
                        check(b, s, "uninit", "UNINIT-READ",
                              bytes ? "use of a value with uninitialised bytes"
                                    : "read of an uninitialised local variable",
                              line);
                    }
                    if (auto u = fr.undef.find(a.var); u != fr.undef.end()) {
                        // (computed from) undef: this use may observe any value
                        // (LangRef "Undefined Values"; refinement finding 4)
                        Arg h = havoc(b, a.width, false, false);
                        if (u->second.is_const) return h;
                        return assign(b, Op::Select, a.width, {u->second, h, a}, "u");
                    }
                }
                return a;
            }
            case ir::Value::Undef:
                return havoc(b, vwidth(o.ty, "operand type"), false, false);
            case ir::Value::Poison: {
                auto w = vwidth(o.ty, "operand type");
                check(b, Arg::c(1, 1), "poison", "UB-POISON",
                      "poison value used (undefined behaviour folded by the front end)", line);
                return havoc(b, w, false, false);
            }
            case ir::Value::Null:
                if (o.ty.kind == ir::Type::Ptr) return Arg::c(kPtrW, 0);
                break;
            case ir::Value::Zero:
                if (o.ty.kind == ir::Type::Ptr) return Arg::c(kPtrW, 0);
                if (auto w = fp::width_of(o.ty)) return Arg::c(*w, 0);
                break;
            case ir::Value::Fp: return Arg::c(vwidth(o.ty, "operand type"), o.v.bits);
            case ir::Value::Global:
                if (o.ty.kind == ir::Type::Ptr) return mt.global(o.v.name);
                break;
            case ir::Value::ConstExpr:
                if (o.v.ce_op == "getelementptr") return mt.const_gep(b, o.v, line);
                if ((o.v.ce_op == "inttoptr" || o.v.ce_op == "bitcast") && o.ty.kind == ir::Type::Ptr &&
                    !o.v.elems.empty() && o.v.elems[0].v.kind == ir::Value::Int && o.v.elems[0].v.bits == 0)
                    return Arg::c(kPtrW, 0);
                throw Unenc{"UNENCODED: constant expression " + o.v.ce_op +
                            (o.v.ce_op == "ptrtoint" ? " (address values are not modelled)" : "")};
            default: break;
        }
        {
                throw Unenc{"UNENCODED: operand " + o.ty.text + " " + o.v.text};
        }
    }

    // The undef shadow of an operand: 1 for a literal undef, the shadow of a
    // value computed from undef, else 0.
    Arg ushadow(Frame& fr, const ir::Operand& o) {
        if (o.v.kind == ir::Value::Undef) return Arg::c(1, 1);
        if (o.v.kind != ir::Value::Local) return Arg::c(1, 0);
        int v = var_of(fr, o.v.name);
        if (v < 0) return Arg::c(1, 0);
        auto it = fr.undef.find(v);
        return it == fr.undef.end() ? Arg::c(1, 0) : it->second;
    }
    bool may_undef(Frame& fr, const ir::Operand& o) {
        Arg u = ushadow(fr, o);
        return !u.is_const || u.bits != 0;
    }
    // A use where an undef value is immediate UB (LangRef: branch on undef,
    // an undef argument or return value with noundef).
    void undef_ub(Frame& fr, const ir::Operand& o, int b, const std::string& what, int line) {
        if (!may_undef(fr, o)) return;
        check(b, ushadow(fr, o), "undef", "UB-POISON", what + " (undefined behaviour)", line);
    }
    static bool noundef(const std::string& attrs) { return attrs.find("noundef") != std::string::npos; }

    // Static may-undef analysis of a frame (fixpoint), then one i1 shadow
    // variable per value that may be computed from undef. Values that
    // never carry undef: freeze (one arbitrary choice), load (undef bytes in
    // memory are the uninitialised-byte shadow), alloca, landingpad, and the
    // result of an inlined call (its return is checked noundef or refused).
    void undef_analysis(Frame& fr) {
        const auto& f = *fr.f;
        std::set<std::string> mu;
        auto is_u = [&](const ir::Operand& o) {
            return o.v.kind == ir::Value::Undef || (o.v.kind == ir::Value::Local && mu.count(o.v.name));
        };
        bool changed = true;
        while (changed) {
            changed = false;
            auto mark = [&](const std::string& n) {
                if (mu.insert(n).second) changed = true;
            };
            for (auto& bl : f.blocks)
                for (auto& in : bl.insts) {
                    if (in.result.empty() || mu.count(in.result)) continue;
                    const auto& op = in.op;
                    if (op == "freeze" || op == "load" || op == "alloca" || op == "landingpad") continue;
                    if (fr.pairs.count(in.result)) {
                        auto& pu = fr.upair[in.result];
                        std::array<bool, 2> nu = pu;
                        if (op == "insertvalue" && in.ops.size() == 2 && in.indices.size() == 1 &&
                            in.indices[0] <= 1) {
                            const auto& agg = in.ops[0];
                            if (agg.v.kind == ir::Value::Undef || agg.v.kind == ir::Value::Poison)
                                nu = {true, true};
                            else if (agg.v.kind == ir::Value::Local && fr.upair.count(agg.v.name))
                                nu = fr.upair[agg.v.name];
                            nu[in.indices[0]] = is_u(in.ops[1]);
                        } else if ((op == "call" || op == "invoke") && (in.callee.empty() || !m.find(in.callee))) {
                            // (an inlined callee's returned fields are checked at its `ret`)
                            for (auto& o : in.ops)
                                if (is_u(o) && !noundef(o.attrs)) nu = {true, true};
                        }
                        if (nu != pu) {
                            pu = nu;
                            changed = true;
                        }
                        continue;
                    }
                    if (op == "phi") {
                        for (auto& [o, _] : in.incoming)
                            if (is_u(o)) {
                                mark(in.result);
                                break;
                            }
                        continue;
                    }
                    if (op == "extractvalue") {
                        if (!in.ops.empty() && in.ops[0].v.kind == ir::Value::Local && in.indices.size() == 1 &&
                            in.indices[0] <= 1)
                            if (auto it = fr.upair.find(in.ops[0].v.name);
                                it != fr.upair.end() && it->second[in.indices[0]])
                                mark(in.result);
                        continue;
                    }
                    if (op == "call" || op == "invoke") {
                        // a defined callee is inlined: undef arguments are checked
                        // (noundef) or refused, its return value is checked
                        if (in.callee.empty() || m.find(in.callee)) continue;
                        for (auto& o : in.ops)
                            if (is_u(o) && !noundef(o.attrs)) {
                                mark(in.result);
                                break;
                            }
                        continue;
                    }
                    for (auto& o : in.ops)
                        if (is_u(o)) {
                            mark(in.result);
                            break;
                        }
                }
        }
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts) {
                if (in.result.empty() || !mu.count(in.result)) continue;
                int v = var_of(fr, in.result);
                if (v < 0) continue;
                // a landing-pad phi's inputs come from throw edges, an
                // extractvalue reads a pair field: both statically undef
                if ((in.op == "phi" && landingpad_of(f, bl.name)) || in.op == "extractvalue") {
                    fr.undef[v] = Arg::c(1, 1);
                    continue;
                }
                fr.undef[v] = Arg::v(newvar(fr.prefix + in.result + ".undef", 1), 1);
            }
    }

    // Set the undef shadow of a non-phi instruction's result: the OR of its
    // operands' shadows (noundef call arguments are checked instead).
    void undef_def(Frame& fr, const ir::Inst& in, int b) {
        if (in.result.empty()) return;
        int v = var_of(fr, in.result);
        if (v < 0) return;
        auto it = fr.undef.find(v);
        if (it == fr.undef.end() || it->second.is_const) return;
        Arg acc = Arg::c(1, 0);
        for (auto& o : in.ops) {
            if ((in.op == "call" || in.op == "invoke") && noundef(o.attrs)) continue;
            Arg u = ushadow(fr, o);
            if (u.is_const && u.bits == 0) continue;
            if (u.is_const || (acc.is_const && acc.bits != 0)) {
                acc = Arg::c(1, 1);
                continue;
            }
            acc = acc.is_const ? u : assign(b, Op::Or, 1, {acc, u}, "u");
        }
        emit_assign(b, it->second.var, Op::Copy, {acc});
    }

    void enter_frame(Frame& fr) {
        const auto& f = *fr.f;
        for (auto& bl : f.blocks) {
            auto q = fr.prefix + bl.name;
            head[q] = newblock(q);
        }
        // the globals the analysed function names: variables before its results (MemTr::preassign_globals)
        if (!frames.empty() && &fr == frames.front()) {
            mt.preassign_globals(f);
            // the hidden allocation-failed flag, when a model can set it: its
            // variable after the globals', its object after theirs (run_frame)
            if (pirmem::reaches_alloc_failed(m, f)) {
                oom_ = Arg::v(newvar("_oom", kPtrW), kPtrW);
                oom_pending_ = true;
            }
        }
        std::set<std::string> maybe_uninit;
        for (auto& bl : f.blocks) {
            for (auto& in : bl.insts) {
                if (in.result.empty()) continue;
                if (in.op == "call" && is_overflow_intrinsic(in.callee)) {
                    if (in.ty.kind == ir::Type::Struct && in.ty.elems.size() == 2 &&
                        in.ty.elems[0].kind == ir::Type::Int && in.ty.elems[0].bits <= 64 &&
                        in.ty.elems[0].bits > 0) {
                        int a = newvar(fr.prefix + in.result + ".0", in.ty.elems[0].bits);
                        int c = newvar(fr.prefix + in.result + ".1", 1);
                        fr.pairs[in.result] = {a, c};
                    }
                    continue;
                }
                if (in.op == "landingpad" && in.ty.kind == ir::Type::Struct &&
                    in.ty.elems.size() == 2 && in.ty.elems[0].kind == ir::Type::Ptr &&
                    in.ty.elems[1].kind == ir::Type::Int && in.ty.elems[1].bits == 32) {
                    // { ptr, i32 }: exception pointer and selector (translate_ctl.inc)
                    int a = newvar(fr.prefix + in.result + ".exn", kPtrW);
                    int s = newvar(fr.prefix + in.result + ".sel", 32);
                    fr.pairs[in.result] = {a, s};
                    fr.defs[in.result] = &in;
                    continue;
                }
                if (in.op == "insertvalue" || in.op == "call" || in.op == "invoke" || in.op == "load")
                    if (auto sp = scalar_pair(in.ty)) {
                        // two-field aggregate (the { ptr, i32 } exception pair
                        // rebuilt before `resume`, a returned std::pair ...)
                        int a = newvar(fr.prefix + in.result + ".0", sp->first);
                        int s = newvar(fr.prefix + in.result + ".1", sp->second);
                        fr.pairs[in.result] = {a, s};
                        fr.defs[in.result] = &in;
                        continue;
                    }
                fr.defs[in.result] = &in;
                auto ty = in.op == "icmp" || in.op == "fcmp" ? ir::Type{ir::Type::Int, 1, "i1", {}} : in.ty;
                if (ty.kind == ir::Type::Ptr && ty.text == "ptr") ty = ir::Type{ir::Type::Int, kPtrW, "ptr", {}};
                bool is_fp = false;
                if (auto fw = fp::width_of(ty)) {
                    ty = ir::Type{ir::Type::Int, *fw, ty.text, {}};  // IEEE bits
                    is_fp = true;
                }
                if (ty.kind == ir::Type::Int && ty.bits > 0 && ty.bits <= 64) {
                    int v = newvar(fr.prefix + in.result, ty.bits);
                    out.vars[static_cast<std::size_t>(v)].fp = is_fp;
                    fr.env[in.result] = Arg::v(v, ty.bits);
                    if (in.op == "call" && starts(in.callee, "__prism.uninit.")) {
                        maybe_uninit.insert(in.result);
                        fr.shadow[v] = Arg::c(1, 1);
                    }
                }
            }
        }
        // phis carrying a maybe-uninitialised value (fixpoint)
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& bl : f.blocks)
                for (auto& in : bl.insts) {
                    if (in.op != "phi" || in.result.empty() || maybe_uninit.count(in.result)) continue;
                    for (auto& [o, _] : in.incoming)
                        if (o.v.kind == ir::Value::Local && maybe_uninit.count(o.v.name)) {
                            maybe_uninit.insert(in.result);
                            changed = true;
                            break;
                        }
                }
        }
        for (auto& bl : f.blocks)
            for (auto& in : bl.insts)
                if (in.op == "phi" && maybe_uninit.count(in.result)) {
                    int v = var_of(fr, in.result);
                    if (v < 0) continue;
                    int sv = newvar(fr.prefix + in.result + ".uninit", 1);
                    fr.shadow[v] = Arg::v(sv, 1);
                }
        undef_analysis(fr);
    }

    // Blocks reachable from the entry along normal edges (br, switch, the
    // normal destination of invoke). Landing pads and the code after them are
    // only entered by an exception: eh_pass (translate_ctl.inc) translates
    // them once a throw edge reaches them.
    static std::set<std::string> normal_blocks(const ir::Function& f) {
        std::map<std::string, const ir::Block*> by;
        for (auto& b : f.blocks) by[b.name] = &b;
        std::set<std::string> seen;
        std::vector<std::string> work{f.blocks.front().name};
        while (!work.empty()) {
            auto n = work.back();
            work.pop_back();
            if (!seen.insert(n).second || !by.count(n)) continue;
            for (auto& in : by[n]->insts) {
                if (in.op == "br" || in.op == "switch")
                    for (auto& t : in.targets) work.push_back(t);
                if (in.op == "switch")
                    for (auto& [c, t] : in.cases) work.push_back(t);
                if (in.op == "invoke" && !in.targets.empty()) work.push_back(in.targets[0]);
            }
        }
        return seen;
    }

    void run_frame(Frame& fr) {
        const auto& f = *fr.f;
        if (&fr == frames.front()) {
            mt.emit_entry_globals();  // first thing of block 0 (prologue)
            if (oom_pending_) {
                mt.alloc(-1, Arg::c(64, 1), MemKind::Static, 1, "allocation failed (PRISM)", 0, oom_->var);
                oom_pending_ = false;
            }
        }
        const auto live = normal_blocks(f);
        for (auto& bl : f.blocks)
            if (live.count(bl.name)) translate_block(fr, bl);
        eh_pass(fr);  // landing pads that a throw reaches, and the code after them
    }

    void translate_block(Frame& fr, const ir::Block& bl) {
        const auto& f = *fr.f;
        {
            auto q = fr.prefix + bl.name;
            int cur = head[q];
            bool noreturn = false;
            bool terminated = false;
            for (auto& in : bl.insts) {
                if (terminated) break;
                auto l = loc(in);
                if (fr.site_line) l = ir::DILoc{fr.site_line, 0};
                int line = l.line;
                if (in.op == "phi" && landingpad_of(f, bl.name)) {
                    // landing pad phi: its predecessors are the throw sites under the
                    // invokes named here (resolved from eh_edges in resolve_phis)
                    int dst = var_of(fr, in.result);
                    if (dst < 0) throw Unenc{"UNENCODED: phi " + in.ty.text};
                    LpadPhi lp{fr.serial, bl.name, cur, dst, {}};
                    for (auto& [o, pred] : in.incoming) {
                        if (o.v.kind == ir::Value::Undef || o.v.kind == ir::Value::Poison)
                            lp.in.emplace_back(pred, havoc(cur, vwidth(o.ty), false, false));
                        else
                            lp.in.emplace_back(pred, operand(fr, o, cur, line, /*use=*/false));
                    }
                    lpad_phis.push_back(std::move(lp));
                    continue;
                }
                if (in.op == "landingpad") continue;  // its { exn, selector } pair is set by the throw edges
                if (in.op == "phi") {
                    int dst = var_of(fr, in.result);
                    if (dst < 0) throw Unenc{"UNENCODED: phi " + in.ty.text};
                    PPhi p;
                    p.block = cur;
                    p.dst = dst;
                    PPhi ps;
                    auto sh = fr.shadow.find(dst);
                    if (sh != fr.shadow.end()) {
                        ps.block = cur;
                        ps.dst = sh->second.var;
                    }
                    PPhi pu;  // undef shadow
                    if (auto uh = fr.undef.find(dst); uh != fr.undef.end() && !uh->second.is_const) {
                        pu.block = cur;
                        pu.dst = uh->second.var;
                    }
                    for (auto& [o, pred] : in.incoming) {
                        int kind = o.v.kind == ir::Value::Undef ? 1 : o.v.kind == ir::Value::Poison ? 2 : 0;
                        Arg a;
                        if (kind == 0) a = operand(fr, o, cur, line, /*use=*/false);
                        else a.width = vwidth(o.ty);
                        p.in.emplace_back(fr.prefix + pred, -1, a, kind);
                        if (ps.dst >= 0) {
                            Arg s = Arg::c(1, 0);
                            if (kind == 0 && !a.is_const)
                                if (auto it = fr.shadow.find(a.var); it != fr.shadow.end()) s = it->second;
                            ps.in.emplace_back(fr.prefix + pred, -1, s, 0);
                        }
                        if (pu.dst >= 0)
                            pu.in.emplace_back(fr.prefix + pred, -1, kind != 0 ? Arg::c(1, 1) : ushadow(fr, o), 0);
                    }
                    pphis.push_back(std::move(p));
                    if (ps.dst >= 0) pphis.push_back(std::move(ps));
                    if (pu.dst >= 0) pphis.push_back(std::move(pu));
                    continue;
                }
                if (in.op == "invoke") {
                    // the call with the landing pad as its exception destination
                    // (a throw inside jumps there: translate_ctl.inc), then the normal edge
                    ir::Inst call = in;
                    call.op = "call";
                    call.targets.clear();
                    handlers.push_back(Handler{&fr, in.targets.at(1), bl.name, frames.size()});
                    bool nr = false;
                    inst(fr, call, cur, l, nr);
                    handlers.pop_back();
                    Term t;
                    if (!nr) {
                        t.kind = Term::Jmp;
                        auto it = head.find(fr.prefix + in.targets.at(0));
                        if (it == head.end()) throw Unenc{"UNENCODED: invoke to unknown block"};
                        t.t = it->second;
                    }
                    out.blocks[static_cast<std::size_t>(cur)].term = t;
                    tail[q] = cur;
                    terminated = true;
                    continue;
                }
                if (in.op == "br" || in.op == "ret" || in.op == "unreachable" || in.op == "switch" ||
                    in.op == "resume") {
                    terminator(fr, in, cur, line, noreturn);
                    tail[q] = cur;
                    terminated = true;
                    continue;
                }
                inst(fr, in, cur, l, noreturn);
                if (noreturn) {
                    // exit/abort/assert: the rest of the block is dead
                    out.blocks[static_cast<std::size_t>(cur)].term = Term{};
                    tail[q] = cur;
                    terminated = true;
                }
            }
            if (!terminated) throw Unenc{"UNENCODED: block without terminator (" + bl.name + ")"};
        }
    }

    void terminator(Frame& fr, const ir::Inst& in, int& cur, int line, bool noreturn) {
        auto& T = out.blocks[static_cast<std::size_t>(cur)].term;
        if (in.op == "switch") throw Unenc{"UNENCODED: switch (lowerswitch did not run)"};
        if (!in.parsed) throw Unenc{"UNENCODED: " + in.op};
        if (in.op == "resume") {
            // re-throw after cleanup code: the exception continues to the next handler
            if (in.ops.empty() || in.ops[0].v.kind != ir::Value::Local || !fr.pairs.count(in.ops[0].v.name))
                throw Unenc{"UNENCODED: resume of " + (in.ops.empty() ? std::string("?") : in.ops[0].ty.text)};
            if (auto pu = fr.upair.find(in.ops[0].v.name); pu != fr.upair.end() && pu->second[0])
                check(cur, Arg::c(1, 1), "undef", "UB-POISON", "resume of an undef exception (undefined behaviour)",
                      line);
            int ev = fr.pairs[in.ops[0].v.name].first;
            raise(cur, Arg::v(ev, kPtrW), std::nullopt, line);
            return;
        }
        if (in.op == "unreachable") {
            if (!noreturn)
                check(cur, Arg::c(1, 1), "unreachable", "CXX-UNREACHABLE",
                      "unreachable code reached (undefined behaviour)", line);
            out.blocks[static_cast<std::size_t>(cur)].term = Term{};
            return;
        }
        if (in.op == "br") {
            auto tgt = [&](const std::string& n) {
                auto it = head.find(fr.prefix + n);
                if (it == head.end()) throw Unenc{"UNENCODED: branch to unknown block " + n};
                return it->second;
            };
            Term t;
            if (in.targets.size() == 1) {
                t.kind = Term::Jmp;
                t.t = tgt(in.targets[0]);
            } else {
                t.kind = Term::Br;
                if (in.ops[0].ty.kind != ir::Type::Int || in.ops[0].ty.bits != 1)
                    throw Unenc{"UNENCODED: br on " + in.ops[0].ty.text};
                undef_ub(fr, in.ops[0], cur, "branch on an undef value", line);
                t.cond = operand(fr, in.ops[0], cur, line);
                t.t = tgt(in.targets[0]);
                t.f = tgt(in.targets[1]);
            }
            out.blocks[static_cast<std::size_t>(cur)].term = t;
            if (!in.loop_md.empty() && fr.prefix.empty())
                if (auto ls = m.loop_starts.find(in.loop_md); ls != m.loop_starts.end()) {
                    out.loop_locs[t.t] = {ls->second.line, ls->second.col};
                    if (t.kind == Term::Br) out.loop_locs[t.f] = {ls->second.line, ls->second.col};
                }
            return;
        }
        if (fr.ret_pair) {
            // an inlined callee returning a scalar pair: both fields go to the
            // caller's pair variables through continuation phis
            if (in.ops.empty() || in.ops[0].v.kind != ir::Value::Local || !fr.pairs.count(in.ops[0].v.name))
                throw Unenc{"UNENCODED: return of " + (in.ops.empty() ? std::string("?") : in.ops[0].ty.text)};
            if (auto pu = fr.upair.find(in.ops[0].v.name);
                pu != fr.upair.end() && (pu->second[0] || pu->second[1]))
                throw Unenc{"UNENCODED: undef field returned from @" + fr.f->name};
            auto [a, s] = fr.pairs[in.ops[0].v.name];
            const std::array<std::pair<int, int>, 2> fields{std::pair{a, fr.ret_pair->first},
                                                            std::pair{s, fr.ret_pair->second}};
            for (unsigned k = 0; k < 2; ++k) {
                const auto [src, dst] = fields[k];
                Arg v = Arg::v(src, out.vars[static_cast<std::size_t>(src)].width);
                if (in.ops[0].ty.elems[k].kind == ir::Type::Ptr) mt.stack_escape_check(cur, v, fr.allocas, line);
            }
            for (auto& al : fr.allocas) mt.end_lifetime(cur, al);  // callee locals end here
            Term t;
            t.kind = Term::Jmp;
            t.t = fr.ret_block;
            out.blocks[static_cast<std::size_t>(cur)].term = t;
            for (const auto& [src, dst] : fields) {
                Arg v = Arg::v(src, out.vars[static_cast<std::size_t>(src)].width);
                bool found = false;
                for (auto& p : pphis)
                    if (p.block == fr.ret_block && p.dst == dst) {
                        p.in.emplace_back("", cur, v, 0);
                        found = true;
                    }
                if (!found) {
                    PPhi p;
                    p.block = fr.ret_block;
                    p.dst = dst;
                    p.in.emplace_back("", cur, v, 0);
                    pphis.push_back(std::move(p));
                }
            }
            return;
        }
        // ret (a raw byte copy is not a use: its shadow goes to the caller)
        std::optional<Arg> v;
        bool raw = !in.ops.empty() && in.ops[0].v.kind == ir::Value::Local && fr.env.count(in.ops[0].v.name) &&
                   !fr.env[in.ops[0].v.name].is_const && fr.raw.count(fr.env[in.ops[0].v.name].var);
        if (!in.ops.empty() && may_undef(fr, in.ops[0])) {
            // LangRef: returning undef from a noundef function is UB. An
            // inlined callee's value would flow on to several uses in the
            // caller as one value: refused unless the return is noundef.
            if (noundef(fr.f->ret_attrs))
                undef_ub(fr, in.ops[0], cur, "undef value returned from a noundef function", line);
            else if (fr.ret_block >= 0)
                throw Unenc{"UNENCODED: undef value returned from @" + fr.f->name + " (no noundef)"};
        }
        if (!in.ops.empty()) v = operand(fr, in.ops[0], cur, line, !raw);
        if (fr.ret_shadow >= 0 && v) {
            Arg s = Arg::c(out.vars[static_cast<std::size_t>(fr.ret_shadow)].width, 0);
            if (raw && fr.shadow[v->var].width == s.width) {
                s = fr.shadow[v->var];
                fr.ret_shadow_used = true;
            }
            PPhi sp;
            bool found = false;
            for (auto& p : pphis)
                if (p.block == fr.ret_block && p.dst == fr.ret_shadow) {
                    p.in.emplace_back("", cur, s, 0);
                    found = true;
                }
            if (!found) {
                sp.block = fr.ret_block;
                sp.dst = fr.ret_shadow;
                sp.in.emplace_back("", cur, s, 0);
                pphis.push_back(std::move(sp));
            }
        }
        if (v && !in.ops.empty() && in.ops[0].ty.kind == ir::Type::Ptr) {
            v->width = kPtrW;
            mt.stack_escape_check(cur, *v, fr.allocas, line);
        }
        if (fr.ret_block >= 0)
            for (auto& a : fr.allocas) mt.end_lifetime(cur, a);  // callee locals end here
        if (fr.ret_block >= 0) {
            Term t;
            t.kind = Term::Jmp;
            t.t = fr.ret_block;
            out.blocks[static_cast<std::size_t>(cur)].term = t;
            if (fr.ret_var >= 0 && v) {
                // continuation phi
                for (auto& p : pphis)
                    if (p.block == fr.ret_block && p.dst == fr.ret_var) {
                        p.in.emplace_back("", cur, *v, 0);
                        return;
                    }
                PPhi p;
                p.block = fr.ret_block;
                p.dst = fr.ret_var;
                p.in.emplace_back("", cur, *v, 0);
                pphis.push_back(std::move(p));
            }
            return;
        }
        Term t;
        t.kind = Term::Ret;
        t.val = v;
        (void)T;
        out.blocks[static_cast<std::size_t>(cur)].term = t;
    }

    int result_var(Frame& fr, const ir::Inst& in) {
        if (in.result.empty()) return -1;
        int v = var_of(fr, in.result);
        if (v < 0) throw Unenc{"UNENCODED: result type " + in.ty.text + " of " + in.op};
        return v;
    }

    Arg p2(int b, Op op, Arg x, Arg y) { return assign(b, op, 1, {x, y}, "c"); }

    void binop(Frame& fr, const ir::Inst& in, int cur, ir::DILoc l) {
        int line = l.line;
        const auto& op = in.op;
        if (op[0] == 'f') throw Unenc{"UNENCODED: " + op + " (floating point)"};
        unsigned w = int_width(in.ty);
        Arg a = operand(fr, in.ops[0], cur, line);
        Arg b = operand(fr, in.ops[1], cur, line);
        a.width = b.width = w;
        if (op == "sub" && is_ptrtoint(fr, in.ops[0]) && is_ptrtoint(fr, in.ops[1])) mt.ptr_sub_check(cur, a, b, line);
        bool nsw = has_flag(in, "nsw"), nuw = has_flag(in, "nuw"), exact = has_flag(in, "exact");
        Op r = Op::Add;
        auto zero = Arg::c(w, 0);
        if (op == "add") {
            r = Op::Add;
            if (nsw) check(cur, p2(cur, Op::SAddOvf, a, b), "ovf+", "INT-SIGNED-OVF", "signed addition overflows", line);
            if (nuw) check(cur, p2(cur, Op::UAddOvf, a, b), "wrap+", "UB-POISON", "add nuw wraps (poison)", line);
        } else if (op == "sub") {
            r = Op::Sub;
            if (nsw) check(cur, p2(cur, Op::SSubOvf, a, b), "ovf-", "INT-SIGNED-OVF", "signed subtraction overflows", line);
            if (nuw) check(cur, p2(cur, Op::USubOvf, a, b), "wrap-", "UB-POISON", "sub nuw wraps (poison)", line);
        } else if (op == "mul") {
            r = Op::Mul;
            if (nsw) check(cur, p2(cur, Op::SMulOvf, a, b), "ovf*", "INT-SIGNED-OVF", "signed multiplication overflows", line);
            if (nuw) check(cur, p2(cur, Op::UMulOvf, a, b), "wrap*", "UB-POISON", "mul nuw wraps (poison)", line);
        } else if (op == "udiv" || op == "urem") {
            r = op == "udiv" ? Op::UDiv : Op::URem;
            check(cur, p2(cur, Op::Eq, b, zero), op == "udiv" ? "div0" : "mod0", "INT-DIV-ZERO",
                  "division by zero", line);
            if (exact) check(cur, p2(cur, Op::InexactU, a, b), "exact", "UB-POISON", "udiv exact is inexact (poison)", line);
        } else if (op == "sdiv" || op == "srem") {
            r = op == "sdiv" ? Op::SDiv : Op::SRem;
            check(cur, p2(cur, Op::Eq, b, zero), op == "sdiv" ? "div0" : "mod0", "INT-DIV-ZERO",
                  "division by zero", line);
            check(cur, p2(cur, Op::SDivOvf, a, b), "divovf", "INT-SIGNED-OVF",
                  "INT_MIN / -1 overflows", line);
            if (exact) check(cur, p2(cur, Op::InexactS, a, b), "exact", "UB-POISON", "sdiv exact is inexact (poison)", line);
        } else if (op == "shl") {
            r = Op::Shl;
            check(cur, p2(cur, Op::ShiftOob, a, b), "shift", "INT-SHIFT-UB", "shift amount >= width", line);
            bool signed_c = std::find(opt.signed_shl.begin(), opt.signed_shl.end(),
                                      std::make_pair(l.line, l.col)) != opt.signed_shl.end();
            if (signed_c)
                check(cur, p2(cur, Op::ShlSOvf, a, b), "shift-base", "INT-SHIFT-UB",
                      "signed left shift of a negative value or into/past the sign bit", line);
            if (nsw) check(cur, p2(cur, Op::ShlNswOvf, a, b), "shl-nsw", "UB-POISON", "shl nsw overflows (poison)", line);
            if (nuw) check(cur, p2(cur, Op::ShlNuwOvf, a, b), "shl-nuw", "UB-POISON", "shl nuw overflows (poison)", line);
        } else if (op == "lshr" || op == "ashr") {
            r = op == "lshr" ? Op::LShr : Op::AShr;
            check(cur, p2(cur, Op::ShiftOob, a, b), "shift", "INT-SHIFT-UB", "shift amount >= width", line);
            if (exact)
                check(cur, p2(cur, op == "lshr" ? Op::LostBitsL : Op::LostBitsA, a, b), "exact", "UB-POISON",
                      op + " exact shifts out set bits (poison)", line);
        } else if (op == "and") {
            r = Op::And;
        } else if (op == "or") {
            r = Op::Or;
            if (has_flag(in, "disjoint")) {
                auto both = assign(cur, Op::And, w, {a, b});
                check(cur, p2(cur, Op::Ne, both, zero), "disjoint", "UB-POISON", "or disjoint with common bits (poison)", line);
            }
        } else if (op == "xor") {
            r = Op::Xor;
        } else {
            throw Unenc{"UNENCODED: " + op};
        }
        int dst = result_var(fr, in);
        if (dst >= 0) emit_assign(cur, dst, r, {a, b});
    }

    void inst(Frame& fr, const ir::Inst& in, int& cur, ir::DILoc l, bool& noreturn) {
        int line = l.line;
        const auto& op = in.op;
        if (!in.parsed) {
            std::string what = op;
            for (auto& fl : in.flags) what += " " + fl;
            throw Unenc{"UNENCODED: " + what};
        }
        undef_def(fr, in, cur);
        if (mem_inst(fr, in, cur, line)) return;
        {
            // floating point (translate_fp.cpp)
            pirfp::Ctx c{[&](std::size_t i) { return operand(fr, in.ops.at(i), cur, line); },
                         [&] { return in.result.empty() ? -1 : result_var(fr, in); }, line};
            if (fpt.inst(cur, in, c)) return;
        }
        if (op == "add" || op == "sub" || op == "mul" || op == "udiv" || op == "sdiv" || op == "urem" ||
            op == "srem" || op == "shl" || op == "lshr" || op == "ashr" || op == "and" || op == "or" ||
            op == "xor" || op == "fadd" || op == "fsub" || op == "fmul" || op == "fdiv" || op == "frem") {
            binop(fr, in, cur, l);
            return;
        }
        if (op == "icmp") {
            if (in.ty.kind == ir::Type::Ptr) {
                if (in.ty.text != "ptr") throw Unenc{"UNENCODED: icmp on " + in.ty.text};
                if (has_flag(in, "samesign")) throw Unenc{"UNENCODED: icmp samesign ptr"};
                Arg a = operand(fr, in.ops[0], cur, line);
                Arg b = operand(fr, in.ops[1], cur, line);
                mt.icmp(cur, result_var(fr, in), in.pred, a, b, line);
                return;
            }
            unsigned w = int_width(in.ty, "icmp operand type");
            Arg a = operand(fr, in.ops[0], cur, line);
            Arg b = operand(fr, in.ops[1], cur, line);
            a.width = b.width = w;
            static const std::map<std::string, Op> preds{
                {"eq", Op::Eq},   {"ne", Op::Ne},   {"ugt", Op::Ugt}, {"uge", Op::Uge}, {"ult", Op::Ult},
                {"ule", Op::Ule}, {"sgt", Op::Sgt}, {"sge", Op::Sge}, {"slt", Op::Slt}, {"sle", Op::Sle}};
            auto it = preds.find(in.pred);
            if (it == preds.end()) throw Unenc{"UNENCODED: icmp " + in.pred};
            if (has_flag(in, "samesign")) {
                // LLVM 19+: the result is poison when the operands' signs
                // differ (refinement finding 2); checked where it is created,
                // like the other poison flags (finding 3)
                Arg na = p2(cur, Op::Slt, a, Arg::c(w, 0));
                Arg nb = p2(cur, Op::Slt, b, Arg::c(w, 0));
                check(cur, p2(cur, Op::Ne, na, nb), "samesign", "UB-POISON",
                      "icmp samesign of operands with different signs (poison)", line);
            }
            emit_assign(cur, result_var(fr, in), it->second, {a, b});
            return;
        }
        if (op == "select") {
            if (in.ops[0].ty.kind != ir::Type::Int || in.ops[0].ty.bits != 1)
                throw Unenc{"UNENCODED: select on " + in.ops[0].ty.text};
            unsigned w = vwidth(in.ty, "select type");
            Arg c = operand(fr, in.ops[0], cur, line);
            Arg a = operand(fr, in.ops[1], cur, line);
            Arg b = operand(fr, in.ops[2], cur, line);
            a.width = b.width = w;
            emit_assign(cur, result_var(fr, in), Op::Select, {c, a, b});
            return;
        }
        if (op == "zext" || op == "sext" || op == "trunc") {
            if (in.ops[0].ty.kind != ir::Type::Int || in.ty.kind != ir::Type::Int)
                throw Unenc{"UNENCODED: " + op + " " + in.ops[0].ty.text + " to " + in.ty.text};
            unsigned fw = int_width(in.ops[0].ty), tw = int_width(in.ty);
            Arg a = operand(fr, in.ops[0], cur, line);
            a.width = fw;
            if (op == "trunc" && (has_flag(in, "nuw") || has_flag(in, "nsw")))
                throw Unenc{"UNENCODED: trunc nuw/nsw"};
            if (op == "zext" && has_flag(in, "nneg"))
                check(cur, p2(cur, Op::Slt, a, Arg::c(fw, 0)), "nneg", "UB-POISON", "zext nneg of a negative value (poison)", line);
            (void)tw;
            emit_assign(cur, result_var(fr, in), op == "zext" ? Op::ZExt : op == "sext" ? Op::SExt : Op::Trunc, {a});
            return;
        }
        if (op == "freeze") {
            unsigned w = vwidth(in.ty, "freeze type");
            // `freeze poison` is defined (LangRef: an arbitrary, fixed value,
            // like `freeze undef`), not a use of poison (refinement finding 5)
            Arg a = in.ops[0].v.kind == ir::Value::Poison ? havoc(cur, w, false, false)
                                                          : operand(fr, in.ops[0], cur, line);
            a.width = w;
            emit_assign(cur, result_var(fr, in), Op::Copy, {a});
            return;
        }
        if (op == "insertvalue") {
            // two-field aggregate: { ptr, i32 } exception pair (rebuilt by
            // clang before `resume`) or a scalar pair (scalar_pair)
            auto it = fr.pairs.find(in.result);
            if (it == fr.pairs.end() || in.indices.size() != 1 || in.indices[0] > 1 || in.ops.size() != 2)
                throw Unenc{"UNENCODED: insertvalue " + in.ty.text};
            const unsigned w0 = out.vars[static_cast<std::size_t>(it->second.first)].width;
            const unsigned w1 = out.vars[static_cast<std::size_t>(it->second.second)].width;
            Arg base0, base1;
            const auto& agg = in.ops[0];
            if (agg.v.kind == ir::Value::Local && fr.pairs.count(agg.v.name)) {
                auto [a, s] = fr.pairs[agg.v.name];
                base0 = Arg::v(a, w0);
                base1 = Arg::v(s, w1);
            } else if (agg.v.kind == ir::Value::Undef || agg.v.kind == ir::Value::Poison) {
                base0 = havoc(cur, w0, false, false);
                base1 = havoc(cur, w1, false, false);
            } else {
                throw Unenc{"UNENCODED: insertvalue into " + agg.v.text};
            }
            Arg v = operand(fr, in.ops[1], cur, line, /*use=*/false);
            v.width = in.indices[0] == 0 ? w0 : w1;
            emit_assign(cur, it->second.first, Op::Copy, {in.indices[0] == 0 ? v : base0});
            emit_assign(cur, it->second.second, Op::Copy, {in.indices[0] == 1 ? v : base1});
            return;
        }
        if (op == "landingpad") return;
        if (op == "extractvalue") {
            if (in.ops.empty() || in.ops[0].v.kind != ir::Value::Local || in.indices.size() != 1)
                throw Unenc{"UNENCODED: extractvalue"};
            auto it = fr.pairs.find(in.ops[0].v.name);
            if (it == fr.pairs.end()) throw Unenc{"UNENCODED: extractvalue of " + in.ops[0].ty.text};
            int src = in.indices[0] == 0 ? it->second.first : it->second.second;
            auto w = out.vars[static_cast<std::size_t>(src)].width;
            int dst = result_var(fr, in);
            if (dst >= 0) emit_assign(cur, dst, Op::Copy, {Arg::v(src, w)});
            return;
        }
        if (op == "call") {
            call(fr, in, cur, l, noreturn);
            return;
        }
        throw Unenc{"UNENCODED: " + op};
    }

    void call(Frame& fr, const ir::Inst& in, int& cur, ir::DILoc l, bool& noreturn) {
        int line = l.line;
        const auto& n = in.callee;
        {
            // undef arguments (refinement finding 4): UB for a noundef
            // parameter; an inlined callee would use the value as one value,
            // so without noundef the call is refused
            const auto* def = n.empty() ? nullptr : m.find(n);
            for (std::size_t i = 0; i < in.ops.size(); ++i) {
                const auto& o = in.ops[i];
                if (!may_undef(fr, o)) continue;
                if (noundef(o.attrs) || (def && i < def->params.size() && noundef(def->params[i].attrs)))
                    undef_ub(fr, o, cur, "undef value passed as a noundef argument", line);
                else if (!in.is_asm && (def || n.empty()))
                    throw Unenc{"UNENCODED: undef value passed to " + (n.empty() ? std::string("an indirect call") : "@" + n) +
                                " (no noundef)"};
            }
            if (in.callee_op) undef_ub(fr, *in.callee_op, cur, "call through an undef function pointer", line);
        }
        if (in.is_asm) {
            asm_call(fr, in, cur, line);
            return;
        }
        if (n.empty()) {
            indirect_call(fr, in, cur, l, noreturn);  // virtual dispatch / function pointers
            return;
        }
        if (eh_call(fr, in, cur, line, noreturn)) return;    // __cxa_* exception runtime
        if (sjlj_call(fr, in, cur, line, noreturn)) return;  // setjmp / longjmp
        auto arg = [&](std::size_t i) {
            if (i >= in.ops.size()) throw Unenc{"UNENCODED: call @" + n + " arity"};
            return operand(fr, in.ops[i], cur, line);
        };
        auto flag_arg = [&](std::size_t i) {
            return i < in.ops.size() && in.ops[i].v.kind == ir::Value::Int && in.ops[i].v.bits != 0;
        };
        {
            // floating-point intrinsics and libm calls (translate_fp.cpp)
            pirfp::Ctx c{[&](std::size_t i) { return arg(i); },
                         [&] { return in.result.empty() ? -1 : result_var(fr, in); }, line};
            if (fpt.call(cur, in, c)) return;
        }
        if (starts(n, "llvm.")) {
            if (is_overflow_intrinsic(n)) {
                auto it = fr.pairs.find(in.result);
                Arg a = arg(0), b = arg(1);
                if (in.result.empty()) return;
                if (it == fr.pairs.end()) throw Unenc{"UNENCODED: call @" + n};
                auto w = out.vars[static_cast<std::size_t>(it->second.first)].width;
                a.width = b.width = w;
                bool s = n[5] == 's';
                auto kind = n.substr(6, 3);  // add / sub / mul
                Op r = kind == "add" ? Op::Add : kind == "sub" ? Op::Sub : Op::Mul;
                Op ov = kind == "add" ? (s ? Op::SAddOvf : Op::UAddOvf)
                        : kind == "sub" ? (s ? Op::SSubOvf : Op::USubOvf)
                                        : (s ? Op::SMulOvf : Op::UMulOvf);
                emit_assign(cur, it->second.first, r, {a, b});
                emit_assign(cur, it->second.second, ov, {a, b});
                return;
            }
            for (auto [p, o] : {std::pair{"llvm.smax.", Op::SMax}, std::pair{"llvm.smin.", Op::SMin},
                                std::pair{"llvm.umax.", Op::UMax}, std::pair{"llvm.umin.", Op::UMin}}) {
                if (starts(n, p)) {
                    unsigned w = int_width(in.ty);
                    Arg a = arg(0), b = arg(1);
                    a.width = b.width = w;
                    emit_assign(cur, result_var(fr, in), o, {a, b});
                    return;
                }
            }
            if (starts(n, "llvm.abs.")) {
                unsigned w = int_width(in.ty);
                Arg a = arg(0);
                a.width = w;
                if (flag_arg(1))
                    check(cur, p2(cur, Op::Eq, a, Arg::c(w, uint64_t{1} << (w - 1))), "abs", "INT-SIGNED-OVF",
                          "abs(INT_MIN) overflows", line);
                emit_assign(cur, result_var(fr, in), Op::Abs, {a});
                return;
            }
            if (starts(n, "llvm.ctlz.") || starts(n, "llvm.cttz.")) {
                unsigned w = int_width(in.ty);
                Arg a = arg(0);
                a.width = w;
                if (flag_arg(1))
                    check(cur, p2(cur, Op::Eq, a, Arg::c(w, 0)), "clz0", "INT-CLZ-ZERO",
                          "count leading/trailing zeros of 0 is undefined", line);
                emit_assign(cur, result_var(fr, in), starts(n, "llvm.ctlz.") ? Op::Ctlz : Op::Cttz, {a});
                return;
            }
            if (starts(n, "llvm.ctpop.") || starts(n, "llvm.bswap.")) {
                unsigned w = int_width(in.ty);
                if (starts(n, "llvm.bswap.") && w % 16 != 0) throw Unenc{"UNENCODED: call @" + n};
                Arg a = arg(0);
                a.width = w;
                emit_assign(cur, result_var(fr, in), starts(n, "llvm.ctpop.") ? Op::Ctpop : Op::Bswap, {a});
                return;
            }
            if (starts(n, "llvm.expect.")) {
                unsigned w = int_width(in.ty);
                Arg a = arg(0);
                a.width = w;
                emit_assign(cur, result_var(fr, in), Op::Copy, {a});
                return;
            }
            if (n == "llvm.assume") {
                // A false llvm.assume is undefined behaviour (LangRef), and
                // clang emits it for __builtin_assume even at -O0. Assuming
                // it silently would discard exactly the inputs that break the
                // program's own contract (refinement finding 8), so it is a
                // checked FUNC-CONTRACT violation first, like a failing
                // assert(); the path then continues under the assumption.
                Arg c = arg(0);
                check(cur, p2(cur, Op::Eq, c, Arg::c(1, 0)), "assume", "FUNC-CONTRACT",
                      "__builtin_assume / llvm.assume condition can be false (undefined behaviour)", line);
                assume(cur, c);
                return;
            }
            if (starts(n, "llvm.memcpy.") || starts(n, "llvm.memmove.")) {
                // the intrinsic allows an exact self-copy (a C memcpy call is
                // not lowered to it: the stage compiles with -fno-builtin-memcpy)
                mt.memcpy_(cur, arg(0), arg(1), arg(2), starts(n, "llvm.memmove."), line, true);
                return;
            }
            if (starts(n, "llvm.memset.")) {
                mt.memset_(cur, arg(0), arg(1), arg(2), line);
                return;
            }
            if (starts(n, "llvm.stacksave")) {
                auto t = mt.stack_save(cur);
                if (int dst = result_var(fr, in); dst >= 0) emit_assign(cur, dst, Op::Copy, {t});
                return;
            }
            if (starts(n, "llvm.lifetime.start") || starts(n, "llvm.lifetime.end")) {
                // (coroutine frames, docs/PIR.md "Coroutines") end: the object's
                // lifetime ends; start: a stack object's lifetime starts again
                // (also after an end) and its bytes become indeterminate
                Arg p = arg(1);
                if (starts(n, "llvm.lifetime.end")) {
                    mt.end_lifetime(cur, p);
                    return;
                }
                if (in.ops.empty() || in.ops[0].v.kind != ir::Value::Int)
                    throw Unenc{"UNENCODED: call @" + n + " (size not a constant)"};
                const uint64_t sz = in.ops[0].v.bits;  // -1: the whole object
                mt.lifetime_start(cur, p, sz == ~uint64_t{0} ? std::nullopt : std::optional<uint64_t>(sz), line);
                return;
            }
            if (starts(n, "llvm.stackrestore")) {
                mt.stack_restore(cur, arg(0));
                return;
            }
            if (starts(n, "llvm.dbg.") || n == "llvm.donothing" || n == "llvm.sideeffect" ||
                starts(n, "llvm.experimental.noalias.scope.decl") || starts(n, "llvm.pseudoprobe"))
                return;
            if (n == "llvm.trap" || n == "llvm.debugtrap" || n == "llvm.ubsantrap") {
                check(cur, Arg::c(1, 1), "trap", "CXX-UNREACHABLE", "trap reached", line);
                noreturn = true;
                return;
            }
            throw Unenc{"UNENCODED: call @" + n};
        }
        if (starts(n, "__prism_")) {
            model_intrinsic(fr, in, cur, line);
            return;
        }
        if (format_call(fr, in, cur, line)) return;
        if (n == "__prism.keep") return;  // keeps a setjmp function's local in memory (lower_ctl.cpp)
        if (starts(n, "__prism.uninit.")) {
            int dst = result_var(fr, in);
            if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, true, false, dst);
            return;
        }
        if (n == "__prism.folded") {
            // marker placed before mem2reg at the instruction carrying a
            // clang UB-folding diagnostic (stage.cpp instrument())
            if (in.ops.empty() || in.ops[0].v.kind != ir::Value::Int) throw Unenc{"UNENCODED: call @" + n};
            auto k = static_cast<std::size_t>(in.ops[0].v.bits);
            std::string cls = "UB-POISON", msg = "undefined behaviour folded by clang";
            if (k < opt.folded.size()) {
                cls = opt.folded[k].cls;
                msg = "undefined behaviour folded by clang: " + opt.folded[k].msg;
                folded_used.insert(static_cast<int>(k));
            }
            check(cur, Arg::c(1, 1), "folded", cls, msg, line);
            return;
        }
        if (starts(n, "__prism.poison.")) {
            check(cur, Arg::c(1, 1), "folded", "UB-POISON",
                  "undefined behaviour folded to poison by clang (e.g. constant division by zero, "
                  "INT_MIN / -1, over-wide constant shift)", line);
            int dst = result_var(fr, in);
            if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
            return;
        }
        const bool defined = m.find(n) != nullptr;
        if (!defined) {
            if (starts(n, "__VERIFIER_nondet_") || starts(n, "nondet_")) {
                int dst = result_var(fr, in);
                if (in.ty.kind != ir::Type::Void && dst < 0) throw Unenc{"UNENCODED: call @" + n};
                if (dst >= 0) {
                    havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, true, dst);
                    if (model_stack.empty() && starts(n, "__VERIFIER_nondet_")) {
                        // a call of the program itself: its value goes into the
                        // counterexample's nondet trace (encode.cpp nondet_trace)
                        auto& s = stmts(cur).back();
                        s.nondet_fn = n;
                        s.nondet_unsigned = nondet_is_unsigned(n);
                        s.line = line;
                        s.col = l.col;
                    }
                }
                out.nondet = true;
                return;
            }
            if (n == "__VERIFIER_assume") {
                Arg c = arg(0);
                assume(cur, p2(cur, Op::Ne, c, Arg::c(c.width, 0)));
                return;
            }
            if (n == "__VERIFIER_error" || n == "reach_error") {
                check(cur, Arg::c(1, 1), "reach_error", "FUNC-CONTRACT", n + "() is reachable", line);
                noreturn = true;
                return;
            }
            if (n == "__assert_fail" || n == "__assert_rtn" || n == "_assert" || n == "__assert" ||
                n == "__assert2" || n == "_wassert") {
                check(cur, Arg::c(1, 1), "assert", "FUNC-CONTRACT", "assertion can fail", line);
                noreturn = true;
                return;
            }
            if (n == "_ZSt21__glibcxx_assert_failPKciS0_S0_" || n == "_ZSt21__glibcxx_assert_failv") {
                // libstdc++ precondition (_GLIBCXX_ASSERTIONS, docs/PIR.md "C++ library")
                std::string cond = in.ops.size() > 3 ? mt.const_string(in.ops[3].v).value_or("") : "";
                std::string where = in.ops.size() > 2 ? mt.const_string(in.ops[2].v).value_or("") : "";
                std::string cls = "FUNC-CONTRACT";
                if (cond.find("size()") != std::string::npos || cond.find("_Nm") != std::string::npos)
                    cls = "MEM-OOB-READ";
                else if (cond.find("_M_is_engaged") != std::string::npos || cond.find("has_value") != std::string::npos)
                    cls = "CXX-OPTIONAL-NULL";
                else if (cond.find("pointer()") != std::string::npos || cond.find("nullptr") != std::string::npos)
                    cls = "PTR-NULL-DEREF";
                check(cur, Arg::c(1, 1), "precondition", cls,
                      "C++ library precondition violated" + (where.empty() ? "" : " in " + where) +
                          (cond.empty() ? "" : ": " + cond),
                      line);
                noreturn = true;
                return;
            }
            if (n == "abort") {
                // Defined behaviour, but a crash: never a clean PROVED path,
                // except out-of-memory handling. On an execution where an
                // allocation already failed (the malloc/calloc/realloc models
                // call __prism_alloc_failed), abort() is the program's answer
                // to the failure, not a defect (docs/PIR.md "Library models",
                // F9; ESBMC lets malloc fail by default and models abort() as
                // assume(false)). Dereferencing an unchecked NULL result is
                // still a separate check.
                Arg viol = Arg::c(1, 1);
                if (oom_) viol = assign(cur, Op::Eq, 1, {ld(cur, *oom_, 8, line), Arg::c(8, 0)}, "c");
                check(cur, viol, "abort", "FUNC-CONTRACT", "abort() is reachable (process crash)", line);
                noreturn = true;
                return;
            }
            if (n == "exit" || n == "_Exit" || n == "_exit" || n == "quick_exit") {
                for (std::size_t i = 0; i < in.ops.size(); ++i) arg(i);
                noreturn = true;
                return;
            }
            throw Unenc{"UNENCODED: call @" + n};
        }
        inline_call(fr, in, cur, line);
    }

    void inline_call(Frame& fr, const ir::Inst& in, int& cur, int line) {
        const auto& n = in.callee;
        const auto* callee = m.find(n);
        if (std::find(stack.begin(), stack.end(), n) != stack.end())
            throw Unenc{"UNENCODED: recursive call @" + n};
        if (static_cast<int>(stack.size()) > opt.inline_depth)
            throw Unenc{"UNENCODED: call depth > " + std::to_string(opt.inline_depth) + " (@" + n + ")"};
        if (!callee->parse_error.empty())
            throw Unenc{"UNENCODED: call @" + n + " (unparsed IR: " + callee->parse_error + ")"};
        const auto ret_pair = scalar_pair(callee->ret);
        if (callee->ret.kind != ir::Type::Void && !ret_pair) vwidth(callee->ret, "call @" + n + " returning");
        std::vector<Arg> args;
        std::vector<char> raw_arg;
        for (std::size_t i = 0; i < in.ops.size(); ++i) {
            const auto& o = in.ops[i];
            bool raw = o.v.kind == ir::Value::Local && fr.env.count(o.v.name) && !fr.env[o.v.name].is_const &&
                       fr.raw.count(fr.env[o.v.name].var);
            raw_arg.push_back(raw);
            args.push_back(operand(fr, o, cur, line, !raw));  // passing raw bytes is no use
        }
        if (args.size() != callee->params.size()) throw Unenc{"UNENCODED: call @" + n + " arity"};
        Frame cf;
        for (std::size_t i = 0; i < args.size(); ++i)
            if (raw_arg[i] && !args[i].is_const) {
                cf.shadow[args[i].var] = fr.shadow[args[i].var];
                cf.raw.insert(args[i].var);
            }
        cf.f = callee;
        cf.prefix = n + "#" + std::to_string(uniq++) + ".";
        cf.serial = ++frame_serial;
        for (std::size_t i = 0; i < args.size(); ++i) {
            auto w = vwidth(callee->params[i].ty, "call @" + n + " parameter");
            args[i].width = w;
            if (auto bt = byval_type(callee->params[i].attrs)) {
                // by-value aggregate: the callee gets its own copy
                auto sz = mt.layout().alloc_size(*bt);
                auto copy = mt.alloc(cur, Arg::c(64, sz), MemKind::Stack, 0, callee->params[i].name, line);
                mt.memcpy_(cur, copy, args[i], Arg::c(64, sz), true, line);
                cf.allocas.push_back(copy);
                args[i] = copy;
            }
            cf.env[callee->params[i].name] = args[i];
        }
        int cont = newblock(fr.prefix + "call." + n + "." + std::to_string(uniq++));
        cf.ret_block = cont;
        if (ret_pair) {
            // a scalar pair (e.g. std::pair<iterator, bool>): two return variables
            if (auto it = fr.pairs.find(in.result); !in.result.empty() && it != fr.pairs.end())
                cf.ret_pair = it->second;
            else
                cf.ret_pair = std::pair{newvar(tmpname("ret"), ret_pair->first), newvar(tmpname("ret"), ret_pair->second)};
        } else if (callee->ret.kind != ir::Type::Void) {
            int rv = in.result.empty() ? newvar(tmpname("ret"), vwidth(callee->ret)) : result_var(fr, in);
            cf.ret_var = rv;
            cf.ret_shadow = newvar(tmpname("retu"), (vwidth(callee->ret) + 7) / 8);
        }
        stack.push_back(n);
        // Library code (a model, or a function from a header such as
        // libstdc++): its checks are reported at the user's call site.
        std::string cfile;
        if (auto it = m.subprograms.find(callee->dbg); it != m.subprograms.end()) cfile = it->second.file;
        bool foreign = callee->is_model || (!cfile.empty() && !top_file.empty() && cfile != top_file);
        if (foreign || fr.site_line) cf.site_line = fr.site_line ? fr.site_line : line;
        bool pushed_model = callee->is_model;
        if (pushed_model) model_stack.push_back(model_display(n));
        out.inlined.push_back(n);
        frames.push_back(&cf);
        enter_frame(cf);
        Term j;
        j.kind = Term::Jmp;
        j.t = head[cf.prefix + callee->blocks.front().name];
        out.blocks[static_cast<std::size_t>(cur)].term = j;
        run_frame(cf);
        frames.pop_back();
        stack.pop_back();
        if (pushed_model) model_stack.pop_back();
        if (cf.ret_shadow_used && cf.ret_var >= 0) {
            fr.shadow[cf.ret_var] = Arg::v(cf.ret_shadow, out.vars[static_cast<std::size_t>(cf.ret_shadow)].width);
            fr.raw.insert(cf.ret_var);
        }
        cur = cont;
    }

    // ---- memory (translate_mem.cpp does the encoding) -------------------------

    static std::optional<ir::Type> byval_type(const std::string& attrs) {
        for (auto* key : {"byval(", "byref("}) {
            auto p = attrs.find(key);
            if (p == std::string::npos) continue;
            auto a = p + std::string_view(key).size();
            int depth = 1;
            auto e = a;
            while (e < attrs.size() && depth > 0) {
                if (attrs[e] == '(') ++depth;
                if (attrs[e] == ')') --depth;
                if (depth > 0) ++e;
            }
            return ir::parse_type(attrs.substr(a, e - a));
        }
        return std::nullopt;
    }

    bool is_ptrtoint(Frame& fr, const ir::Operand& o) {
        if (o.v.kind != ir::Value::Local) return false;
        auto it = fr.defs.find(o.v.name);
        return it != fr.defs.end() && it->second->op == "ptrtoint";
    }

    // alloca/load/store/getelementptr/ptrtoint/inttoptr/bitcast; false = not a memory instruction
    // Field k of the scalar pair `ty` stored at p: fn(k, pointer, alignment).
    template <typename Fn>
    void pair_fields(int cur, const ir::Type& ty, Arg p, unsigned align, int line, Fn fn) {
        const auto& lay = mt.layout();
        const uint64_t off1 = lay.field_offset(ty, 1);
        const unsigned a0 = align ? align : lay.align(ty);
        // the second field's alignment: what the struct's alignment and the
        // field offset guarantee
        unsigned a1 = a0;
        while (a1 > 1 && off1 % a1 != 0) a1 /= 2;
        fn(0, p, a0);
        int q = newvar(tmpname("pair.f1"), kPtrW);
        mt.gep(cur, q, p, ty, {Arg::c(32, 0), Arg::c(32, 1)}, true, line, 1);
        fn(1, Arg::v(q, kPtrW), a1);
    }

    bool mem_inst(Frame& fr, const ir::Inst& in, int cur, int line) {
        const auto& op = in.op;
        if (op == "alloca") {
            int dst = result_var(fr, in);
            std::optional<Arg> count, src;
            if (!in.ops.empty()) {
                count = operand(fr, in.ops[0], cur, line);
                count->width = int_width(in.ops[0].ty, "alloca count");
                if (in.ops[0].v.kind == ir::Value::Local)
                    if (auto it = fr.defs.find(in.ops[0].v.name);
                        it != fr.defs.end() && (it->second->op == "zext" || it->second->op == "sext") &&
                        !it->second->ops.empty() && it->second->ops[0].ty.kind == ir::Type::Int) {
                        src = operand(fr, it->second->ops[0], cur, line);
                        src->width = int_width(it->second->ops[0].ty);
                    }
            }
            mt.alloca_(cur, dst, in.ety, in.align, count, src, in.result, line);
            if (!count && fr.f && !fr.f->blocks.empty() && cur == head[fr.prefix + fr.f->blocks.front().name])
                fr.allocas.push_back(Arg::v(dst, kPtrW));
            return true;
        }
        if (op == "load" && fr.pairs.count(in.result)) {
            // a scalar pair: its two fields are loaded one by one (the
            // padding between them is not read)
            if (in.ops.empty() || in.ops[0].ty.kind != ir::Type::Ptr || in.ops[0].ty.text != "ptr")
                throw Unenc{"UNENCODED: load"};
            Arg p = operand(fr, in.ops[0], cur, line);
            auto [a, s] = fr.pairs[in.result];
            pair_fields(cur, in.ty, p, in.align, line, [&](unsigned k, Arg fp, unsigned al) {
                mt.load(cur, k == 0 ? a : s, in.ty.elems[k], fp, al, line);
            });
            return true;
        }
        if (op == "store" && in.ops.size() == 2 && in.ops[0].v.kind == ir::Value::Local &&
            fr.pairs.count(in.ops[0].v.name) && scalar_pair(in.ops[0].ty)) {
            if (in.ops[1].ty.kind != ir::Type::Ptr || in.ops[1].ty.text != "ptr") throw Unenc{"UNENCODED: store"};
            auto [a, s] = fr.pairs[in.ops[0].v.name];
            Arg p = operand(fr, in.ops[1], cur, line);
            pair_fields(cur, in.ops[0].ty, p, in.align, line, [&](unsigned k, Arg fp, unsigned al) {
                int v = k == 0 ? a : s;
                mt.store(cur, fp, Arg::v(v, out.vars[static_cast<std::size_t>(v)].width), Arg::c(1, 1),
                         in.ops[0].ty.elems[k], al, line);
            });
            return true;
        }
        if (op == "load") {
            if (in.ops.empty() || in.ops[0].ty.kind != ir::Type::Ptr || in.ops[0].ty.text != "ptr")
                throw Unenc{"UNENCODED: load"};
            Arg p = operand(fr, in.ops[0], cur, line);
            vwidth(in.ty, "load of");
            // An integer load of a whole aggregate object (ABI coercion of a
            // struct passed or returned by value) copies bytes: padding may be
            // uninitialised without a defect, so its shadow follows the value
            // to its uses instead of being checked here.
            bool agg = false;
            if (in.ty.kind == ir::Type::Int && in.ops[0].v.kind == ir::Value::Local) {
                // static pointee type of the loaded location: the alloca's
                // type, or the type a getelementptr selects ("coerce.dive")
                if (auto it = fr.defs.find(in.ops[0].v.name); it != fr.defs.end()) try {
                    const auto* d = it->second;
                    std::optional<ir::Type> pt;
                    if (d->op == "alloca") {
                        pt = d->ety;
                    } else if (d->op == "getelementptr") {
                        const ir::Type* t = &d->ety;
                        bool ok = true;
                        for (std::size_t k = 2; ok && k < d->ops.size(); ++k) {
                            const auto& rt = mt.layout().resolve(*t);
                            if (rt.kind == ir::Type::Struct && d->ops[k].v.kind == ir::Value::Int)
                                t = &mt.layout().field_type(rt, static_cast<unsigned>(d->ops[k].v.bits));
                            else if (rt.kind == ir::Type::Array)
                                t = &rt.elems.at(0);
                            else
                                ok = false;
                        }
                        if (ok) pt = *t;
                    }
                    if (pt) {
                        const auto& rt = mt.layout().resolve(*pt);
                        agg = rt.kind == ir::Type::Struct || rt.kind == ir::Type::Array;
                    }
                } catch (const Unenc&) {
                    agg = false;  // opaque type: an ordinary checked load
                }
            }
            int dst = result_var(fr, in);
            int sh = mt.load(cur, dst, in.ty, p, in.align, line, !agg);
            if (agg && dst >= 0) {
                fr.shadow[dst] = Arg::v(sh, out.vars[static_cast<std::size_t>(sh)].width);
                fr.raw.insert(dst);
            }
            return true;
        }
        if (op == "store") {
            if (in.ops.size() != 2 || in.ops[1].ty.kind != ir::Type::Ptr || in.ops[1].ty.text != "ptr")
                throw Unenc{"UNENCODED: store"};
            auto w = vwidth(in.ops[0].ty, "store of");
            Arg init = Arg::c(1, 1);
            Arg v;
            const auto& vo = in.ops[0];
            if (vo.v.kind == ir::Value::Undef || vo.v.kind == ir::Value::Poison) {
                v = havoc(cur, w, false, false);
                init = Arg::c(1, 0);  // storing an indeterminate value
            } else {
                v = operand(fr, vo, cur, line, /*use=*/false);  // copying a maybe-uninitialised value is no use
                if (!v.is_const)
                    if (auto sh = fr.shadow.find(v.var); sh != fr.shadow.end()) {
                        const auto& s = sh->second;
                        if (s.width == 1)
                            init = assign(cur, Op::Eq, 1, {s, Arg::c(1, 0)}, "init");
                        else  // per-byte mask of a raw copy: initialised = not uninitialised
                            init = assign(cur, Op::Xor, s.width, {s, Arg::c(s.width, ~uint64_t{0})}, "init");
                    }
                if (may_undef(fr, vo)) {
                    // storing (a value computed from) undef writes indeterminate bytes
                    if (init.width != 1) throw Unenc{"UNENCODED: store of a raw copy computed from undef"};
                    Arg u = ushadow(fr, vo);
                    Arg d = u.is_const ? Arg::c(1, 0) : assign(cur, Op::Eq, 1, {u, Arg::c(1, 0)}, "init");
                    init = init.is_const ? (init.bits != 0 ? d : init) : assign(cur, Op::And, 1, {init, d}, "init");
                }
            }
            Arg p = operand(fr, in.ops[1], cur, line);
            mt.store(cur, p, v, init, vo.ty, in.align, line);
            return true;
        }
        if (op == "getelementptr") {
            if (in.ty.kind != ir::Type::Ptr || in.ops.empty() || in.ops[0].ty.text != "ptr")
                throw Unenc{"UNENCODED: vector getelementptr"};
            Arg base = operand(fr, in.ops[0], cur, line);
            std::vector<Arg> idx;
            for (std::size_t k = 1; k < in.ops.size(); ++k) {
                Arg x = operand(fr, in.ops[k], cur, line);
                x.width = int_width(in.ops[k].ty, "getelementptr index");
                idx.push_back(x);
            }
            // how the result is used: 0 address, 1 loaded, 2 stored through, 3 deeper GEP
            int use = 0;
            for (auto& bl : fr.f->blocks)
                for (auto& u : bl.insts) {
                    auto is_res = [&](std::size_t i) {
                        return i < u.ops.size() && u.ops[i].v.kind == ir::Value::Local && u.ops[i].v.name == in.result;
                    };
                    if (u.op == "load" && is_res(0)) use = std::max(use, 1);
                    if (u.op == "store" && is_res(1)) use = std::max(use, 2);
                    if (u.op == "getelementptr" && is_res(0)) use = std::max(use, 3);
                }
            // base = the last field of a struct (flexible array member / struct hack)?
            bool base_last = false;
            if (in.ops[0].v.kind == ir::Value::Local)
                if (auto it = fr.defs.find(in.ops[0].v.name); it != fr.defs.end() && it->second->op == "getelementptr")
                    try {
                        const auto* d = it->second;
                        const ir::Type* t = &d->ety;
                        for (std::size_t k = 2; k < d->ops.size(); ++k) {
                            const auto& rt = mt.layout().resolve(*t);
                            if (rt.kind == ir::Type::Struct && d->ops[k].v.kind == ir::Value::Int) {
                                auto fi = static_cast<unsigned>(d->ops[k].v.bits);
                                base_last = fi + 1 == rt.elems.size();
                                t = &mt.layout().field_type(rt, fi);
                            } else if (rt.kind == ir::Type::Array) {
                                base_last = false;
                                t = &rt.elems.at(0);
                            } else {
                                break;
                            }
                        }
                    } catch (const Unenc&) {
                        base_last = true;  // unknown layout: no sub-array check
                    }
            mt.gep(cur, result_var(fr, in), base, in.ety, idx, has_flag(in, "inbounds"), line, use, base_last);
            return true;
        }
        if (op == "ptrtoint") {
            // Only pointer differences are modelled: every use must be a sub
            // of two ptrtoint values (checked to be in the same object).
            if (in.ty.kind != ir::Type::Int || in.ty.bits != 64)
                throw Unenc{"UNENCODED: ptrtoint (address values are not modelled)"};
            for (auto& bl : fr.f->blocks)
                for (auto& u : bl.insts) {
                    bool uses = false;
                    for (auto& o : u.ops)
                        if (o.v.kind == ir::Value::Local && o.v.name == in.result) uses = true;
                    for (auto& [o, _] : u.incoming)
                        if (o.v.kind == ir::Value::Local && o.v.name == in.result) uses = true;
                    if (!uses) continue;
                    if (u.op != "sub" || u.ops.size() != 2 || !is_ptrtoint(fr, u.ops[0]) || !is_ptrtoint(fr, u.ops[1]))
                        throw Unenc{"UNENCODED: ptrtoint (address values are not modelled)"};
                }
            Arg p = operand(fr, in.ops[0], cur, line);
            emit_assign(cur, result_var(fr, in), Op::Copy, {p});
            return true;
        }
        if (op == "inttoptr") {
            const auto& o = in.ops[0];
            if (o.v.kind == ir::Value::Int && o.v.bits == 0) {
                emit_assign(cur, result_var(fr, in), Op::Copy, {Arg::c(kPtrW, 0)});
                return true;
            }
            throw Unenc{"UNENCODED: inttoptr (integer-to-pointer casts are not modelled)"};
        }
        if (op == "bitcast" && in.ty.kind == ir::Type::Ptr && in.ops[0].ty.kind == ir::Type::Ptr) {
            emit_assign(cur, result_var(fr, in), Op::Copy, {operand(fr, in.ops[0], cur, line)});
            return true;
        }
        return false;
    }

    // Model intrinsics used by the libc / libc++ operational models
    // (src/prism/pir/models/, docs/PIR.md "Library models").
    void model_intrinsic(Frame& fr, const ir::Inst& in, int& cur, int line) {
        const auto& n = in.callee;
        auto arg = [&](std::size_t i) {
            if (i >= in.ops.size()) throw Unenc{"UNENCODED: call @" + n + " arity"};
            return operand(fr, in.ops[i], cur, line);
        };
        auto cint = [&](std::size_t i) -> uint64_t {
            if (i >= in.ops.size() || in.ops[i].v.kind != ir::Value::Int)
                throw Unenc{"UNENCODED: call @" + n + " with a non-constant argument"};
            return in.ops[i].v.bits;
        };
        auto kind_what = [](MemKind k) -> std::string {
            switch (k) {
                case MemKind::New: return "delete";
                case MemKind::NewArr: return "delete[]";
                case MemKind::File: return "fclose()";
                default: return "free()";
            }
        };
        int dst = in.result.empty() ? -1 : result_var(fr, in);
        if (n == "__prism_alloc") {
            auto k = static_cast<MemKind>(cint(1));
            auto what = k == MemKind::New ? "operator new" : k == MemKind::NewArr ? "operator new[]"
                        : k == MemKind::File ? "FILE" : "heap allocation";
            mt.alloc(cur, arg(0), k, static_cast<int>(cint(2)), what, line, dst);
            return;
        }
        if (n == "__prism_free" || n == "__prism_free_check") {
            auto k = static_cast<MemKind>(cint(1));
            mt.dealloc(cur, arg(0), k, kind_what(k), line, n == "__prism_free");
            return;
        }
        if (n == "__prism_check") {
            Arg ok = arg(0);
            auto cls = mt.const_string(in.ops.at(1).v), msg = mt.const_string(in.ops.at(2).v);
            if (!cls || !msg) throw Unenc{"UNENCODED: __prism_check without literal class/message"};
            check(cur, assign(cur, Op::Eq, 1, {ok, Arg::c(ok.width, 0)}, "c"), "model", *cls, *msg, line);
            return;
        }
        if (n == "__prism_assume") {
            Arg c = arg(0);
            assume(cur, assign(cur, Op::Ne, 1, {c, Arg::c(c.width, 0)}, "c"));
            return;
        }
        if (n == "__prism_alloc_failed") {
            // hidden flag object, zero at entry: set when a library allocation fails
            if (!oom_) oom_ = mt.alloc(-1, Arg::c(64, 1), MemKind::Static, 1, "allocation failed (PRISM)", 0);
            st(cur, *oom_, Arg::c(8, 1), line);
            return;
        }
        if (n == "__prism_obj_size") {
            auto r = mt.obj_size_remaining(cur, arg(0));
            if (dst >= 0) emit_assign(cur, dst, Op::Copy, {r});
            return;
        }
        if (n == "__prism_memcpy") {
            mt.memcpy_(cur, arg(0), arg(1), arg(2), cint(3) != 0, line);
            return;
        }
        if (n == "__prism_memset") {
            mt.memset_(cur, arg(0), arg(1), arg(2), line);
            return;
        }
        if (n == "__prism_read_range") {
            mt.read_range(cur, arg(0), arg(1), line);
            return;
        }
        if (n == "__prism_havoc_bytes") {
            mt.write_havoc(cur, arg(0), arg(1), line);
            return;
        }
        if (n == "__prism_fresh_cstr") {
            auto p = mt.fresh_cstr(cur, static_cast<MemKind>(cint(0)), line);
            if (dst >= 0) emit_assign(cur, dst, Op::Copy, {p});
            return;
        }
        throw Unenc{"UNENCODED: call @" + n};
    }

    // printf family (format checks: %n, argument count and types); false = not handled
    bool format_call(Frame& fr, const ir::Inst& in, int& cur, int line);

    // Inline a module function with the given arguments (used for string
    // validity checks of %s arguments: strlen model).
    void inline_named(Frame& fr, const std::string& name, const std::vector<ir::Operand>& ops, int& cur, int line) {
        ir::Inst call;
        call.op = "call";
        call.callee = name;
        call.ty = ir::Type{ir::Type::Void, 0, "void", {}};
        call.ops = ops;
        if (!m.find(name)) throw Unenc{"UNENCODED: call @" + name + " (no model)"};
        inline_call(fr, call, cur, line);
    }

    void resolve_phis() {
        for (auto& lp : lpad_phis) {
            // one incoming per throw edge, valued by the invoke it passed through
            PPhi p;
            p.block = lp.block;
            p.dst = lp.dst;
            for (auto& e : eh_edges)
                if (e.frame == lp.frame && e.lpad == lp.lpad)
                    for (auto& [pred, v] : lp.in)
                        if (pred == e.invoke_block) {
                            p.in.emplace_back("", e.pred, v, 0);
                            break;
                        }
            pphis.push_back(std::move(p));
        }
        for (auto& p : pphis) {
            Phi phi;
            phi.dst = p.dst;
            for (auto& [qname, idx, a, kind] : p.in) {
                int pred = idx;
                if (pred < 0) {
                    auto it = tail.find(qname);
                    if (it == tail.end()) continue;  // predecessor never translated (dead)
                    pred = it->second;
                }
                Arg v = a;
                if (kind == 2) {
                    // Poison flowing along this edge: PRISM's strict semantics makes
                    // creating/propagating poison a violation (the same rule as a
                    // poison operand use above). Without this check a poison phi that
                    // later reaches br/ret was UB in LLVM but failed no PIR check
                    // (found by the Lean refinement proof, docs/PROOFS_REFINEMENT.md).
                    check(pred, Arg::c(1, 1), "poison", "UB-POISON",
                          "poison value flows into a phi (undefined behaviour folded by the front end)", 0);
                }
                if (kind != 0) {
                    // undef / poison incoming: an unconstrained value on that edge
                    v = havoc(pred, a.width, false, false);
                }
                phi.in.emplace_back(pred, v);
            }
            out.blocks[static_cast<std::size_t>(p.block)].phis.push_back(std::move(phi));
        }
    }
};

#include "translate_ctl.inc"

bool Tr::format_call(Frame& fr, const ir::Inst& in, int& cur, int line) {
    const auto& n = in.callee;
    if (m.find(n) || !pirmem::is_format_function(n)) return false;
    std::vector<ir::Type> tys;
    for (auto& o : in.ops) tys.push_back(o.ty);
    std::optional<std::string> fmt;
    auto fi = n == "printf" ? 0u : n == "snprintf" ? 2u : 1u;
    if (fi < in.ops.size()) fmt = mt.const_string(in.ops[fi].v);
    std::vector<std::optional<uint64_t>> lits;
    for (auto& o : in.ops) lits.push_back(o.v.kind == ir::Value::Int ? std::optional<uint64_t>(o.v.bits) : std::nullopt);
    auto plan = pirmem::plan_format(n, fmt, tys, lits);
    if (!plan.handled) return false;
    if (!plan.unencoded.empty()) throw Unenc{plan.unencoded};
    std::vector<Arg> args;
    for (auto& o : in.ops) {
        if (o.ty.kind == ir::Type::Float) {
            args.push_back(Arg::c(1, 0));  // a double argument is only type-checked
            continue;
        }
        args.push_back(operand(fr, o, cur, line));
    }
    for (auto& [cls, msg] : plan.violations) check(cur, Arg::c(1, 1), "format", cls, msg, line);
    for (auto i : plan.cstr_args) {
        Arg a = args[i];
        a.width = kPtrW;
        check(cur, assign(cur, Op::Eq, 1, {assign(cur, Op::LShr, 64, {a, Arg::c(64, kObjShift)}), Arg::c(64, 0)}),
              "null", "PTR-NULL-DEREF", n + " %s argument is a null pointer", line);
        inline_named(fr, "strlen", {in.ops[i]}, cur, line);
    }
    if (plan.buf_arg >= 0) {
        Arg buf = args[static_cast<std::size_t>(plan.buf_arg)];
        buf.width = kPtrW;
        Arg len = havoc(cur, 64, false, false);
        if (plan.size_arg >= 0) {
            Arg sz = args[static_cast<std::size_t>(plan.size_arg)];
            if (sz.width < 64) sz = assign(cur, Op::ZExt, 64, {sz});
            // snprintf(buf, 0, ...) writes nothing; otherwise at most size-1 characters and a NUL
            int wb = newblock("snprintf.write"), done = newblock("snprintf.done");
            Term t;
            t.kind = Term::Br;
            t.cond = assign(cur, Op::Ne, 1, {sz, Arg::c(64, 0)});
            t.t = wb;
            t.f = done;
            out.blocks[static_cast<std::size_t>(cur)].term = t;
            cur = wb;
            assume(cur, assign(cur, Op::Ult, 1, {len, sz}));
            mt.write_havoc(cur, buf, len, line);
            int end = newvar(tmpname("end"), kPtrW);
            mt.gep(cur, end, buf, ir::Type{ir::Type::Int, 8, "i8", {}}, {len}, true, line);
            mt.store(cur, Arg::v(end, kPtrW), Arg::c(8, 0), Arg::c(1, 1), ir::Type{ir::Type::Int, 8, "i8", {}}, 1, line);
            Term j;
            j.kind = Term::Jmp;
            j.t = done;
            out.blocks[static_cast<std::size_t>(cur)].term = j;
            cur = done;
        } else {
            assume(cur, assign(cur, Op::Ule, 1, {len, Arg::c(64, plan.max_len)}));
            mt.write_havoc(cur, buf, len, line);
            int end = newvar(tmpname("end"), kPtrW);
            mt.gep(cur, end, buf, ir::Type{ir::Type::Int, 8, "i8", {}}, {len}, true, line);
            mt.store(cur, Arg::v(end, kPtrW), Arg::c(8, 0), Arg::c(1, 1), ir::Type{ir::Type::Int, 8, "i8", {}}, 1, line);
        }
    }
    if (int dst = in.result.empty() ? -1 : result_var(fr, in); dst >= 0)
        havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
    return true;
}

// Pointer parameter bound to a contract object (Law 6 relaxation, docs/PIR.md).
void bind_contract(Tr& tr, const ir::Function& f, const ir::Param& p, const PtrContract& c, Frame& top) {
    uint64_t esz = pirmem::contract_elem_bytes(tr.mt.layout(), f, p, c);
    if (!esz)
        throw Unenc{"UNENCODED: element size of pointer parameter " + p.name + " unknown (contract " + c.text + ")"};
    Arg size;
    std::string count_text;
    if (c.count >= 0) {
        if (static_cast<uint64_t>(c.count) > kMaxObjSize / esz) throw Unenc{"UNENCODED: contract object too large"};
        size = Arg::c(64, static_cast<uint64_t>(c.count) * esz);
        count_text = std::to_string(c.count);
    } else {
        int pv = -1;
        for (std::size_t i = 0; i < f.params.size(); ++i)
            if (f.params[i].name == c.count_param && f.params[i].ty.kind == ir::Type::Int) {
                auto it = top.env.find(c.count_param);
                if (it != top.env.end() && !it->second.is_const) pv = it->second.var;
            }
        if (pv < 0) throw Unenc{"UNENCODED: contract size " + c.count_param + " is not an integer parameter"};
        Arg n = Arg::v(pv, tr.out.vars[static_cast<std::size_t>(pv)].width);
        if (n.width < 64) n = tr.assign(-1, Op::SExt, 64, {n});
        if (c.count_add) n = tr.assign(-1, Op::Add, 64, {n, Arg::c(64, static_cast<uint64_t>(c.count_add))});
        if (c.count_min != INT64_MIN || c.count_max != INT64_MAX) {
            // drafted size range (the draft's own assumption, listed)
            tr.assume(-1, tr.assign(-1, Op::Sge, 1, {n, Arg::c(64, static_cast<uint64_t>(c.count_min))}));
            tr.assume(-1, tr.assign(-1, Op::Sle, 1, {n, Arg::c(64, static_cast<uint64_t>(c.count_max))}));
            tr.out.assumptions.push_back(std::to_string(c.count_min) + " <= " + c.count_param +
                                         " <= " + std::to_string(c.count_max) + " (drafted size range)");
        }
        Arg pos = tr.assign(-1, Op::Sgt, 1, {n, Arg::c(64, 0)});
        Arg nn = tr.assign(-1, Op::Select, 64, {pos, n, Arg::c(64, 0)});
        tr.assume(-1, tr.assign(-1, Op::Ult, 1, {nn, Arg::c(64, kMaxObjSize / esz)}));
        size = tr.assign(-1, Op::Mul, 64, {nn, Arg::c(64, esz)});
        count_text = c.count_param + (c.count_add ? (c.count_add > 0 ? "+" : "") + std::to_string(c.count_add) : "");
        tr.out.assumptions.push_back(count_text + " * " + std::to_string(esz) + " < 2^47 (object size fits the model)");
    }
    auto obj = tr.mt.alloc(-1, size, c.read_only ? MemKind::Const : MemKind::Extern, 2, "*" + p.name + " (contract)", 0);
    top.env[p.name] = obj;
    tr.out.ptr_params.push_back(p.name);
    tr.out.assumptions.push_back(p.name + " points to the start of a valid object of max(" + count_text + ", 0) x " +
                                 std::to_string(esz) + " bytes, 16-byte aligned, initialised with arbitrary values (" +
                                 c.source + ": " + c.text + ")");
}

}  // namespace

Translation translate(const ir::Module& m, const ir::Function& f, const TranslateOptions& opt) {
    Translation t;
    t.status = std::string(laws::NEEDS_HARNESS);
    if (!f.parse_error.empty()) {
        t.reason = "UNENCODED: unparsed IR (" + f.parse_error + ")";
        return t;
    }
    auto contract_of = [&](const std::string& name) -> const PtrContract* {
        for (auto& c : opt.contracts)
            if (c.param == name) return &c;
        return nullptr;
    };
    auto own_object = [](const std::string& attrs) {
        return attrs.find("byval(") != std::string::npos || attrs.find("sret(") != std::string::npos;
    };
    for (auto& p : f.params) {
        if (p.ty.kind == ir::Type::Ptr && !own_object(p.attrs) && !contract_of(p.name)) {
            t.reason = "pointer parameter: unguarded model checking reports missing preconditions, "
                       "not defects (Law 6)";
            return t;
        }
    }
    try {
        Tr tr(m, opt);
        tr.out.ir_name = f.name;
        tr.out.name = f.name;
        Frame top;
        top.f = &f;
        std::vector<const ir::Param*> ptrs;
        for (auto& p : f.params) {
            if (p.ty.kind == ir::Type::Ptr) {
                ptrs.push_back(&p);
                continue;
            }
            auto w = p.ty.kind == ir::Type::Float ? tr.vwidth(p.ty, "parameter type") : int_width(p.ty, "parameter type");
            int v = tr.newvar(p.name.empty() ? tr.tmpname("arg") : p.name, w);
            tr.out.vars[static_cast<std::size_t>(v)].fp = p.ty.kind == ir::Type::Float;
            tr.out.params.push_back(v);
            top.env[p.name] = Arg::v(v, w);
        }
        int contracts = 0;
        for (auto* p : ptrs) {
            auto sret = p->attrs.find("sret(");
            if (auto bt = Tr::byval_type(p->attrs); bt || sret != std::string::npos) {
                // by-value aggregate (the caller's copy, initialised) or the
                // return slot (uninitialised): objects the language provides
                ir::Type ty = bt ? *bt : ir::parse_type(p->attrs.substr(sret + 5, p->attrs.find(')', sret) - sret - 5));
                auto sz = tr.mt.layout().alloc_size(ty);
                top.env[p->name] = tr.mt.alloc(-1, Arg::c(64, sz), MemKind::Stack, bt ? 2 : 0, p->name, 0);
                continue;
            }
            bind_contract(tr, f, *p, *contract_of(p->name), top);
            ++contracts;
        }
        if (contracts > 1)
            tr.out.assumptions.push_back("pointer parameters point to distinct objects (\\separated)");
        if (f.ret.kind != ir::Type::Void) {
            tr.out.ret_width = tr.vwidth(f.ret, "return type");
            tr.out.returns_ptr = f.ret.kind == ir::Type::Ptr;
        }
        if (f.blocks.empty()) throw Unenc{"UNENCODED: empty function body"};
        tr.stack.push_back(f.name);
        if (auto it = m.subprograms.find(f.dbg); it != m.subprograms.end()) tr.top_file = it->second.file;
        top.serial = ++tr.frame_serial;
        tr.frames.push_back(&top);
        tr.enter_frame(top);
        tr.run_frame(top);
        tr.resolve_phis();
        if (!tr.prologue.empty()) {
            auto& b0 = tr.out.blocks.front().stmts;
            b0.insert(b0.begin(), tr.prologue.begin(), tr.prologue.end());
        }
        t.fn = std::move(tr.out);
        t.folded_used.assign(tr.folded_used.begin(), tr.folded_used.end());
        t.status.clear();
    } catch (const Unenc& u) {
        t.reason = u.reason;
    }
    return t;
}

}  // namespace prism::pir
