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
#include "callee_contract.hpp"
#include "harness.hpp"
#include "translate_fp.hpp"
#include "translate_mem.hpp"
#include "models/asm/x86_64_asm.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace prism::pir {
namespace {

using pirmem::Unenc;
using pirmem::ptr_w;

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
    // --kernel-api: the result of an inline asm with three or more direct outputs ({ i32, i32, i32, i32 } of cpuid),
    // one variable per field; only extractvalue reads it (asm_havoc writes every field)
    std::map<std::string, std::vector<int>> tuples;
    // --pir-kernel-current: integer asm outputs that hold a pointer (Linux `current`: this_cpu_read_stable(current_task))
    std::set<std::string> ptr_ints;
    // a scalar pair load's field-1 address, numbered between its two fields
    // (the order export_lean's `L pload` gives the Lean frame)
    std::map<std::string, int> pairq;
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

struct PPhiIn {
    std::string qname;
    int idx = -1;
    Arg a;
    int kind = 0;  // 0 value, 1 undef, 2 poison
};

struct PPhi {
    int block = -1;
    int dst = -1;
    std::vector<PPhiIn> in;
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
    // format-call temporaries: per translation (not a process-wide static),
    // so translations on other threads cannot race on it or change the names
    unsigned fmt_seq = 0;
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
    bool oom_pending_ = false;
    int caught_var_ = -1;                             // caught_'s preassigned variable                        // oom_ preassigned, its object not yet emitted

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
    // kernel mode (translate_kernel.inc, docs/KERNEL_MODELS.md)
    void note_assumption(const std::string& text);
    void asm_havoc(Frame& fr, const ir::Inst& in, int& cur, int line, const std::string& tag = "asm-havoc", const std::string& extra = "");
    bool asm_exact(Frame& fr, const ir::Inst& in, int& cur, int line);
    void unknown_call(Frame& fr, const ir::Inst& in, int& cur, int line);
    // --pir-icall-havoc: an indirect call not resolved to inlined targets is a havoc of all memory (translate_kernel.inc)
    Arg icall_havoc(int& cur, const ir::Inst& in, unsigned w, int line, const std::string& why);
    Arg opaque_ptr(int& cur, const std::string& what, int line);
    // ghost lock state (translate_lock.inc, docs/KERNEL_CONCURRENCY.md)
    std::optional<Arg> lk_;                           // hidden object: held locks, atomic depth
    int lk_var_ = -1;                                 // lk_'s preassigned variable (object emitted in run_frame)
    bool lk_pending_ = false;
    static bool reaches_lock_state(const ir::Module& m, const ir::Function& f);
    static int lock_effects(const ir::Module& m, const ir::Function& f);
    int lock_wrapper_ = 0;      // --pir-lock-wrappers: 1 acquire-only root, 2 release-only root, 0 neither
    std::string root_name_;     // the analysed function
    void lock_return_check(int b, int line);
    void emit_lock_state();
    Arg lock_word(int b, unsigned i, int line);
    void lock_take(int b, Arg p, Arg spin, int line);
    void lock_drop(int b, Arg p, Arg spin, int line);
    void sleep_point(int b, int line);
    void sleep_if(int b, Arg may, int line);
    void lock_bump(int b, unsigned w, Arg delta, int line);
    Arg lock_contains(int b, unsigned base, Arg p, int line);
    void lock_insert(int b, unsigned base, Arg p, Arg fresh, int line);
    void lock_declare(int b, bool entry, Arg p, Arg spin, int line);
    void lock_table(Frame& fr);
    bool lock_contracted(const std::string& fn) const;
    void atomic_add(int b, Arg delta, int line);
    void rcu_add(int b, Arg delta, int line);
    bool lock_root(const Frame& fr) const { return !frames.empty() && &fr == frames.front(); }

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
        if (b < 0) return prologue;
        return out.blocks.at(static_cast<std::size_t>(b)).stmts;
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
        // --pir-discharge: only the call-site obligations are properties of this translation
        if (!opt.obligations.empty() && cls != "HARNESS-OBLIGATION") return;
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
            if (e.kind == ir::Type::Ptr && e.text == "ptr") return ptr_w();
            if (e.kind == ir::Type::Int && e.bits > 0 && e.bits <= 64) return e.bits;
            if (e.kind == ir::Type::Float && e.text == "double") return 64u;  // std::complex<double>
            if (e.kind == ir::Type::Float && e.text == "float") return 32u;
            return 0;
        };
        unsigned a = w(t.elems[0]), b = w(t.elems[1]);
        if (!a || !b) return std::nullopt;
        return std::pair{a, b};
    }

    // The fields of a { double, double } pair are floating-point variables.
    void mark_fp_pair(const ir::Type& t, int a, int b) {
        if (t.kind != ir::Type::Struct || t.elems.size() != 2) return;
        out.vars[static_cast<std::size_t>(a)].fp = t.elems[0].kind == ir::Type::Float;
        out.vars[static_cast<std::size_t>(b)].fp = t.elems[1].kind == ir::Type::Float;
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
                if (o.ty.kind == ir::Type::Ptr) return Arg::c(ptr_w(), 0);
                break;
            case ir::Value::Zero:
                if (o.ty.kind == ir::Type::Ptr) return Arg::c(ptr_w(), 0);
                if (auto w = fp::width_of(o.ty)) return Arg::c(*w, 0);
                break;
            case ir::Value::Fp: return Arg::c(vwidth(o.ty, "operand type"), o.v.bits);
            case ir::Value::Global:
                if (o.ty.kind == ir::Type::Ptr) return mt.global(o.v.name);
                break;
            case ir::Value::ConstExpr:
                if (o.v.ce_op == "getelementptr") return mt.const_gep(b, o.v, line);
                if (opt.addr_values && (o.v.ce_op == "ptrtoint" || o.v.ce_op == "inttoptr" || o.v.ce_op == "bitcast"))
                    return mt.const_addr(b, o.v, line);  // --pir-addr-values
                if ((o.v.ce_op == "inttoptr" || o.v.ce_op == "bitcast") && o.ty.kind == ir::Type::Ptr &&
                    !o.v.elems.empty() && o.v.elems[0].v.kind == ir::Value::Int && o.v.elems[0].v.bits == 0)
                    return Arg::c(ptr_w(), 0);
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
                            nu.at(in.indices[0]) = is_u(in.ops[1]);
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
                oom_ = Arg::v(newvar("_oom", ptr_w()), ptr_w());
                oom_pending_ = true;
            }
            // the hidden caught-exceptions stack, when a handler can be entered:
            // allocated eagerly (after oom_), so its object is the LLVM side's too (pir-lean)
            if (pirmem::reaches_caught_slot(m, f)) caught_var_ = newvar("_caught", ptr_w());
            // the ghost lock state, when a lock spec can reach it (translate_lock.inc)
            if (opt.lock_wrappers && !lock_contracted(f.name)) {
                const int fx = lock_effects(m, f);
                lock_wrapper_ = fx == 1 ? 1 : fx == 2 ? 2 : 0;
                root_name_ = f.name;
            }
            if (reaches_lock_state(m, f) || lock_contracted(f.name)) {
                lk_var_ = newvar("_lock", ptr_w());
                lk_pending_ = true;
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
                    int a = newvar(fr.prefix + in.result + ".exn", ptr_w());
                    int s = newvar(fr.prefix + in.result + ".sel", 32);
                    fr.pairs[in.result] = {a, s};
                    fr.defs[in.result] = &in;
                    continue;
                }
                if (in.op == "call" && in.is_asm && !opt.kernel_api.empty() && in.ty.kind == ir::Type::Struct &&
                    in.ty.elems.size() > 2) {
                    bool scalar = true;
                    for (auto& e : in.ty.elems)
                        scalar = scalar && ((e.kind == ir::Type::Int && e.bits > 0 && e.bits <= 64) || e.kind == ir::Type::Ptr);
                    if (scalar) {
                        std::vector<int> vs;
                        for (std::size_t k = 0; k < in.ty.elems.size(); ++k)
                            vs.push_back(newvar(fr.prefix + in.result + "." + std::to_string(k),
                                                in.ty.elems[k].kind == ir::Type::Ptr ? ptr_w() : in.ty.elems[k].bits));
                        fr.tuples[in.result] = std::move(vs);
                        fr.defs[in.result] = &in;
                        continue;
                    }
                }
                if (in.op == "insertvalue" || in.op == "call" || in.op == "invoke" || in.op == "load")
                    if (auto sp = scalar_pair(in.ty)) {
                        // two-field aggregate (the { ptr, i32 } exception pair
                        // rebuilt before `resume`, a returned std::pair ...)
                        int a = newvar(fr.prefix + in.result + ".0", sp->first);
                        if (in.op == "load") fr.pairq[in.result] = newvar(tmpname("pair.f1"), ptr_w());
                        int s = newvar(fr.prefix + in.result + ".1", sp->second);
                        mark_fp_pair(in.ty, a, s);
                        fr.pairs[in.result] = {a, s};
                        fr.defs[in.result] = &in;
                        continue;
                    }
                fr.defs[in.result] = &in;
                auto ty = in.op == "icmp" || in.op == "fcmp" ? ir::Type{ir::Type::Int, 1, "i1", {}} : in.ty;
                if (ty.kind == ir::Type::Ptr && ty.text == "ptr") ty = ir::Type{ir::Type::Int, ptr_w(), "ptr", {}};
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
                if (in.op == "br" || in.op == "switch" || in.op == "callbr")
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
            if (caught_var_ >= 0 && !caught_)
                caught_ = mt.alloc(-1, Arg::c(64, 72), MemKind::Static, 1, "caught exceptions (PRISM)", 0, caught_var_);
            if (lk_pending_) emit_lock_state();  // translate_lock.inc
            if (lk_) lock_table(fr);            // --lock-contracts of this root (translate_lock.inc)
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
                        p.in.push_back(PPhiIn{fr.prefix + pred, -1, a, kind});
                        if (ps.dst >= 0) {
                            Arg s = Arg::c(1, 0);
                            if (kind == 0 && !a.is_const)
                                if (auto it = fr.shadow.find(a.var); it != fr.shadow.end()) s = it->second;
                            ps.in.push_back(PPhiIn{fr.prefix + pred, -1, s, 0});
                        }
                        if (pu.dst >= 0)
                            pu.in.push_back(
                                PPhiIn{fr.prefix + pred, -1, kind != 0 ? Arg::c(1, 1) : ushadow(fr, o), 0});
                    }
                    pphis.push_back(std::move(p));
                    if (ps.dst >= 0) pphis.push_back(std::move(ps));
                    if (pu.dst >= 0) pphis.push_back(std::move(pu));
                    continue;
                }
                if (in.op == "callbr") {
                    // asm goto: the asm (havoc, translate_kernel.inc), then a nondeterministic jump to the
                    // fallthrough block or to the one indirect label (docs/KERNEL_MODELS.md)
                    if (opt.kernel_api.empty())
                        throw Unenc{"UNENCODED: asm goto (--kernel-api models it as a nondeterministic jump)"};
                    if (!in.parsed || in.targets.empty())
                        throw Unenc{"UNENCODED: asm goto without a parsed label list"};
                    auto tgt = [&](const std::string& n) {
                        auto it = head.find(fr.prefix + n);
                        if (it == head.end()) throw Unenc{"UNENCODED: asm goto to unknown block " + n};
                        return it->second;
                    };
                    ir::Inst call = in;
                    call.op = "call";
                    call.targets.clear();
                    bool nr = false;
                    inst(fr, call, cur, l, nr);
                    const std::size_t nind = in.targets.size() - 1;  // indirect labels
                    Term t;
                    t.kind = Term::Br;
                    t.cond = havoc(cur, 1, false, false);
                    if (nind == 1) {
                        t.t = tgt(in.targets[0]);
                        t.f = tgt(in.targets[1]);
                    } else {
                        // chain: cur picks label 1 or goes on; block k picks label k+1 or goes on; the last picks
                        // the fallthrough. Every label is reachable; the order is nondeterministic, not a guess.
                        std::vector<int> chain{cur};
                        for (std::size_t k = 1; k < nind; ++k)
                            chain.push_back(newblock(fr.prefix + "asmgoto." + std::to_string(line) + "." +
                                                     std::to_string(k)));
                        for (std::size_t k = 0; k < nind; ++k) {
                            const int b = chain[k];
                            Term tk;
                            tk.kind = Term::Br;
                            tk.cond = (b == cur) ? t.cond : havoc(b, 1, false, false);
                            tk.t = tgt(in.targets[k + 1]);
                            tk.f = (k + 1 < nind) ? chain[k + 1] : tgt(in.targets[0]);
                            out.blocks[static_cast<std::size_t>(b)].term = tk;
                        }
                        t = out.blocks[static_cast<std::size_t>(cur)].term;
                    }
                    note_assumption("asm-goto: \"" + in.asm_constraints + "\" (line " + std::to_string(line) +
                                    "): the asm continues at the next statement or jumps to one of its " +
                                    std::to_string(nind) + " label(s), any of them");
                    out.blocks[static_cast<std::size_t>(cur)].term = t;
                    tail[q] = cur;
                    terminated = true;
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
                if (in.op == "call" && in.is_asm && opt.kernel_stops && !opt.kernel_api.empty() &&
                    in.asm_text.find("__bug_table") != std::string::npos) {
                    // --pir-kernel-stops: Linux BUG() is `asm volatile(ASM_UD2 (.byte 0x0f, 0x0b) + a __bug_table entry);
                    // __builtin_unreachable()`. Only that shape: the asm followed (after other asm statements only, such
                    // as annotations) by unreachable. WARN() uses the same asm and continues, so it never matches.
                    // At -O0 the unreachable is often in the next block (`br label %n` then `n: unreachable`): follow
                    // unconditional branches, a few hops, over result-less asm only.
                    const ir::Block* cb = &bl;
                    auto k = static_cast<std::size_t>(&in - bl.insts.data()) + 1;
                    bool reaches = false;
                    for (int hops = 0; hops < 4; ++hops) {
                        while (k < cb->insts.size() && cb->insts[k].op == "call" && cb->insts[k].is_asm &&
                               cb->insts[k].result.empty())
                            ++k;
                        if (k >= cb->insts.size()) break;
                        const auto& nx = cb->insts[k];
                        if (nx.op == "unreachable") {
                            reaches = true;
                            break;
                        }
                        if (nx.op != "br" || nx.targets.size() != 1) break;
                        const ir::Block* to = nullptr;
                        for (auto& b2 : fr.f->blocks)
                            if (b2.name == nx.targets[0]) to = &b2;
                        if (!to) break;
                        cb = to;
                        k = 0;
                    }
                    if (reaches) {
                        note_assumption("kernel-stop: assuming the BUG() / BUG_ON() condition never triggers (line " + std::to_string(line) +
                                        "): BUG() (ud2 with a __bug_table entry, then unreachable) stops the kernel, the paths that reach "
                                        "it are not checked, and the verdict holds only for executions in which it never triggers");
                        out.blocks[static_cast<std::size_t>(cur)].term = Term{};
                        tail[q] = cur;
                        terminated = true;
                        continue;
                    }
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
        if (in.op == "switch") throw Unenc{"UNENCODED: switch (lower-switch did not run)"};
        if (!in.parsed) throw Unenc{"UNENCODED: " + in.op};
        if (in.op == "resume") {
            // re-throw after cleanup code: the exception continues to the next handler
            if (in.ops.empty() || in.ops[0].v.kind != ir::Value::Local || !fr.pairs.count(in.ops[0].v.name))
                throw Unenc{"UNENCODED: resume of " + (in.ops.empty() ? std::string("?") : in.ops[0].ty.text)};
            if (auto pu = fr.upair.find(in.ops[0].v.name); pu != fr.upair.end() && pu->second[0])
                check(cur, Arg::c(1, 1), "undef", "UB-POISON", "resume of an undef exception (undefined behaviour)",
                      line);
            int ev = fr.pairs[in.ops[0].v.name].first;
            raise(cur, Arg::v(ev, ptr_w()), std::nullopt, line);
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
                        p.in.push_back(PPhiIn{"", cur, v, 0});
                        found = true;
                    }
                if (!found) {
                    PPhi p;
                    p.block = fr.ret_block;
                    p.dst = dst;
                    p.in.push_back(PPhiIn{"", cur, v, 0});
                    pphis.push_back(std::move(p));
                }
            }
            return;
        }
        if (fr.ret_block < 0 && !in.ops.empty() && scalar_pair(in.ops[0].ty)) {
            // the analysed function returning a scalar pair: each pointer
            // field's stack-escape check, then a return without a value (PIR's
            // `ret` carries one; the fields' checks are what is observed)
            if (in.ops[0].v.kind != ir::Value::Local || !fr.pairs.count(in.ops[0].v.name))
                throw Unenc{"UNENCODED: return of " + in.ops[0].ty.text};
            if (auto pu = fr.upair.find(in.ops[0].v.name);
                pu != fr.upair.end() && (pu->second[0] || pu->second[1]))
                throw Unenc{"UNENCODED: undef field returned"};
            auto [a, s] = fr.pairs[in.ops[0].v.name];
            for (unsigned k = 0; k < 2; ++k) {
                int src = k == 0 ? a : s;
                if (in.ops[0].ty.elems[k].kind == ir::Type::Ptr)
                    mt.stack_escape_check(cur, Arg::v(src, out.vars[static_cast<std::size_t>(src)].width),
                                          fr.allocas, line);
            }
            lock_return_check(cur, line);
            Term t;
            t.kind = Term::Ret;
            out.blocks[static_cast<std::size_t>(cur)].term = t;
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
        if (fr.ret_shadow >= 0 && v.has_value()) {
            Arg s = Arg::c(out.vars[static_cast<std::size_t>(fr.ret_shadow)].width, 0);
            if (raw && fr.shadow[v->var].width == s.width) {
                s = fr.shadow[v->var];
                fr.ret_shadow_used = true;
            }
            PPhi sp;
            bool found = false;
            for (auto& p : pphis)
                if (p.block == fr.ret_block && p.dst == fr.ret_shadow) {
                    p.in.push_back(PPhiIn{"", cur, s, 0});
                    found = true;
                }
            if (!found) {
                sp.block = fr.ret_block;
                sp.dst = fr.ret_shadow;
                sp.in.push_back(PPhiIn{"", cur, s, 0});
                pphis.push_back(std::move(sp));
            }
        }
        if (v && !in.ops.empty() && in.ops[0].ty.kind == ir::Type::Ptr) {
            v->width = ptr_w();
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
                        p.in.push_back(PPhiIn{"", cur, *v, 0});
                        return;
                    }
                PPhi p;
                p.block = fr.ret_block;
                p.dst = fr.ret_var;
                p.in.push_back(PPhiIn{"", cur, *v, 0});
                pphis.push_back(std::move(p));
            }
            return;
        }
        lock_return_check(cur, line);  // LOCK-HELD-ON-RETURN (translate_lock.inc)
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
            if (signed_c && opt.wrapv) {
                // --pir-build-semantics, -fwrapv build: a signed << wraps like an unsigned one (only the amount is checked)
            } else if (signed_c && opt.shl_cxx) {
                check(cur, p2(cur, Op::ShlNuwOvf, a, b), "shift-base", "INT-SHIFT-UB",
                      "signed left shift whose result does not fit the unsigned type (C++ before C++20)", line);
                check(cur, p2(cur, Op::Slt, a, zero), "shift-base", "INT-SHIFT-UB",
                      "signed left shift of a negative value", line);
            } else if (signed_c)
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
        if (!in.kernel_note.empty()) {
            // atomics and barriers: modelled only with --kernel-api, each use a named assumption
            if (opt.kernel_api.empty())
                throw Unenc{"UNENCODED: atomic or barrier instruction (" + in.kernel_note + "; --kernel-api models it)"};
            note_assumption("atomics-sequential: " + in.kernel_note +
                            " (no other thread, interrupt or device changes memory between this function's accesses)");
        } else if (!opt.kernel_api.empty() && (op == "load" || op == "store") &&
                   std::ranges::find(in.flags, std::string("volatile")) != in.flags.end()) {
            note_assumption("volatile-access: volatile (READ_ONCE/WRITE_ONCE/MMIO) accesses are ordinary reads and writes "
                            "of the object in program order (no concurrent or hardware change between accesses)");
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
            if (is_ptrtoint(fr, in.ops[0]) && is_ptrtoint(fr, in.ops[1])) {  // std::less<T*>
                mt.addr_cmp(cur, result_var(fr, in), in.pred, operand(fr, in.ops[0], cur, line),
                            operand(fr, in.ops[1], cur, line));
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
            // trunc nuw / nsw (LLVM 19+): poison when the dropped bits are not
            // all zero (nuw) / not a sign extension (nsw). Checked, never trusted.
            if (op == "trunc" && (has_flag(in, "nuw") || has_flag(in, "nsw"))) {
                Arg t = assign(cur, Op::Trunc, tw, {a}, "t");
                if (has_flag(in, "nuw"))
                    check(cur, p2(cur, Op::Ne, assign(cur, Op::ZExt, fw, {t}, "t"), a), "nuw", "UB-POISON",
                          "trunc nuw drops nonzero bits (poison)", line);
                if (has_flag(in, "nsw"))
                    check(cur, p2(cur, Op::Ne, assign(cur, Op::SExt, fw, {t}, "t"), a), "nsw", "UB-POISON",
                          "trunc nsw changes the signed value (poison)", line);
            }
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
            if (auto tu = fr.tuples.find(in.ops[0].v.name); tu != fr.tuples.end()) {
                if (in.indices[0] >= tu->second.size()) throw Unenc{"UNENCODED: extractvalue"};
                const int src = tu->second[in.indices[0]];
                int dst = result_var(fr, in);
                if (dst >= 0) emit_assign(cur, dst, Op::Copy, {Arg::v(src, out.vars[static_cast<std::size_t>(src)].width)});
                return;
            }
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
        if (opt.kernel_stops && !opt.kernel_api.empty() && (n == "panic" || n == "vpanic" || n == "kassert_panic")) {
            const auto* def = m.find(n);
            if (!def || def->is_model || def->blocks.empty()) {
                // --pir-kernel-stops: the kernel does not return from panic(9) / KASSERT's kassert_panic
                note_assumption("kernel-stop: assuming the @" + n + " condition never triggers (line " + std::to_string(line) +
                                "): the kernel does not return from @" + n + ", the paths that call it are not checked, and the verdict "
                                "holds only for executions in which it is never called");
                noreturn = true;
                return;
            }
        }
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
            // kernel code has no libm: its `log`, `abs`, ... are the kernel's (docs/KERNEL_MODELS.md)
            if ((opt.kernel_api.empty() || starts(n, "llvm.")) && fpt.call(cur, in, c)) return;
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
            if (n == "llvm.returnaddress" || n == "llvm.frameaddress.p0") {
                // a code / frame address: any address outside every modelled
                // object (object id 0, any offset; null for an outer frame is
                // offset 0). Not a pointer into the program's data, so it does
                // not alias a local (no stack escape) and a dereference is
                // checked like a wild pointer.
                Arg h = havoc(cur, ptr_w(), false, true);
                emit_assign(cur, result_var(fr, in), Op::And, {h, Arg::c(ptr_w(), off_mask())});
                return;
            }
            if (starts(n, "llvm.is.constant.")) {
                // __builtin_constant_p: false is always a correct answer (LLVM's
                // lower-constant-intrinsics folds what it cannot prove to false;
                // the guarded code is the general path either way)
                emit_assign(cur, result_var(fr, in), Op::Copy, {Arg::c(1, 0)});
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
            if (opt.builtin_contracts && !opt.kernel_api.empty()) {
                // --pir-builtin-contracts (docs/HARNESS_PHASE2.md): contracts of the body-less intrinsics the kernel headers emit
                const int dst = in.result.empty() ? -1 : result_var(fr, in);
                if (starts(n, "llvm.read_register.")) {
                    // __builtin_read_register / current_stack_pointer: the value of a hardware register
                    if (dst >= 0) {
                        if (in.ty.kind == ir::Type::Ptr || in.ty.kind == ir::Type::Int) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
                        else throw Unenc{"UNENCODED: call @" + n + " (result of type " + in.ty.text + ")"};
                    }
                    note_assumption("builtin-contract: @llvm.read_register reads a hardware register: its value is arbitrary (a pointer result is an "
                                    "arbitrary value that points to no object); it changes no memory");
                    return;
                }
                if (starts(n, "llvm.write_register.")) {
                    // Only the stack pointer: the write-back clang emits after an asm with a "+r"(current_stack_pointer)
                    // operand (Linux ASM_CALL_CONSTRAINT), which writes back the value the asm statement left there.
                    // The stack is not addressed through registers in this model, so the write changes nothing it
                    // sees; any other register stays UNENCODED.
                    std::string reg;
                    if (!in.ops.empty() && in.ops[0].v.kind == ir::Value::Other)
                        if (auto it = m.md_strings.find(in.ops[0].v.text); it != m.md_strings.end()) reg = it->second;
                    if (reg != "rsp" && reg != "esp" && reg != "sp") throw Unenc{"UNENCODED: call @" + n + (reg.empty() ? "" : " (register " + reg + ")")};
                    note_assumption("builtin-contract: @llvm.write_register of the stack pointer " + reg +
                                    " (the write-back of an asm \"+r\"(current_stack_pointer) operand) leaves the stack pointer as the asm "
                                    "statement left it: no effect on the program state the model sees");
                    return;
                }
                if (starts(n, "llvm.prefetch.")) {
                    note_assumption("builtin-contract: @llvm.prefetch has no effect on the program state (a hint: the address is not accessed, "
                                    "so it need not be valid)");
                    return;
                }
                if (starts(n, "llvm.objectsize.")) {
                    // __builtin_object_size / __builtin_dynamic_object_size: the size of the object, or the "unknown" constant
                    if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
                    note_assumption("builtin-contract: @llvm.objectsize returns an arbitrary value (the object size or the unknown marker): "
                                    "fortify-style checks that depend on it are not decided");
                    return;
                }
            }
            throw Unenc{"UNENCODED: call @" + n};
        }
        if (starts(n, "__prism_")) {
            model_intrinsic(fr, in, cur, line);
            return;
        }
        // the kernel's printf/snprintf take extensions (%pX, %D, %b): the kernel specs answer them
        if (opt.kernel_api.empty() && format_call(fr, in, cur, line)) return;
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
            if (k < opt.folded.size() && opt.wrapv && wraps_under_fwrapv(opt.folded[k])) {
                folded_used.insert(static_cast<int>(k));
                return;  // -fwrapv build: clang's folded signed overflow / signed shift is the wrapped value, not UB
            }
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
        // SV-COMP error sinks: a reachable call is the violation whatever the
        // unit's definition does (a static no-op reach_error included).
        if ((n == "__VERIFIER_error" || n == "reach_error") && in.ops.empty()) {
            check(cur, Arg::c(1, 1), "reach_error", "FUNC-CONTRACT", n + "() is reachable", line);
            // the call site is checked here, as if the sink were inlined: a
            // defined sink is discharged to this caller (caller_discharged_asserts)
            if (defined) out.inlined.push_back(n);
            noreturn = true;
            return;
        }
        if (!defined) {
            if (n == "assume_abort_if_not" && in.ops.size() == 1) {
                // SV-COMP helper declared without a body: assume(cond)
                Arg c = arg(0);
                assume(cur, p2(cur, Op::Ne, c, Arg::c(c.width, 0)));
                return;
            }
            if (n == "__VERIFIER_assert" && in.ops.size() == 1) {
                // declared without a body: SV-COMP semantics at the call site
                Arg c = arg(0);
                check(cur, p2(cur, Op::Eq, c, Arg::c(c.width, 0)), "assert", "FUNC-CONTRACT",
                      "__VERIFIER_assert can fail", line);
                return;
            }
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
            // C23 `static_assert(e[, msg])` in a unit lowered as C17 (no
            // <assert.h> macro): clang sees a call to an undeclared function.
            // Its operand is the constant expression C23 6.7.11 requires to be
            // nonzero; failing it is a constraint violation, checked here (a
            // constant true operand proves, anything else is reported).
            if (n == "static_assert" && !in.ops.empty()) {
                Arg c = arg(0);
                check(cur, p2(cur, Op::Eq, c, Arg::c(c.width, 0)), "static_assert", "FUNC-CONTRACT",
                      "static_assert operand can be zero", line);
                int dst = result_var(fr, in);
                if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
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
            if (n == "abort" && std::ranges::find(stack, std::string("assume_abort_if_not")) != stack.end()) {
                // inside SV-COMP's assume_abort_if_not: abort() ends the path
                // (an assumption, as SV-COMP defines it), not a crash
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
            if (auto cc = opt.callee_contracts.find(n); cc != opt.callee_contracts.end()) {
                // --callee-contracts (docs/HARNESS_PHASE2.md): requires checked here, assigns havocked, ensures assumed
                std::vector<Arg> sargs;
                std::vector<ir::Type> stys;
                for (auto& o : in.ops) {
                    sargs.push_back(operand(fr, o, cur, line));
                    stys.push_back(o.ty);
                }
                pirmem::StubCall sc;
                sc.callee = n;
                sc.args = std::move(sargs);
                sc.arg_types = std::move(stys);
                sc.ret = in.ty;
                sc.line = line;
                auto so = pirmem::apply_callee_contract(*this, mt, cc->second, sc, cur);
                if (so.applied) {
                    note_assumption(so.assumption);
                    if (!in.result.empty()) {
                        if (int dst = result_var(fr, in); dst >= 0 && so.result) emit_assign(cur, dst, Op::Copy, {*so.result});
                    }
                    return;
                }
                // not applied (reason in so.why): the call is handled exactly as if there were no contract
            }
            if (opt.assume_unknown_calls) {  // opt-in: result and pointed-to objects havocked (translate_kernel.inc)
                unknown_call(fr, in, cur, line);
                return;
            }
            throw Unenc{"UNENCODED: call @" + n};
        }
        if (auto ob = opt.obligations.find(n); ob != opt.obligations.end()) call_obligations(fr, in, ob->second, cur, line);
        inline_call(fr, in, cur, line);
    }

    // --pir-discharge: the callee's automatic harness as properties of this call site (docs/HARNESS_GENERATOR.md).
    // The pointer arguments must be what the harness assumed: a live, writable, 16-aligned object of the stated
    // size and alignment, initialised, the pointer at the stated offset into it, the nested pointer fields likewise, and every
    // object of the call distinct from every other.
    void oblige_node(Arg p, const ObligationNode& nd, const std::string& callee, std::vector<Arg>& objs, int cur,
                     int line) {
        const std::string what = "call of @" + callee + ": argument " + nd.name;
        mt.obligation_object(cur, p, nd.bytes, nd.member_off, nd.align, nd.check_init, what, line);
        objs.push_back(p);
        for (const auto& [off, child] : nd.nested) {
            int slot = newvar(tmpname("obslot"), ptr_w());
            mt.gep(cur, slot, p, ir::parse_type("i8"), {Arg::c(64, off)}, true, line);
            int val = newvar(tmpname("obptr"), ptr_w());
            mt.load(cur, val, ir::parse_type("ptr"), Arg::v(slot, ptr_w()), 1, line, false);
            oblige_node(Arg::v(val, ptr_w()), *child, callee, objs, cur, line);
        }
    }
    void call_obligations(Frame& fr, const ir::Inst& in, const std::vector<std::shared_ptr<ObligationNode>>& ob,
                          int& cur, int line) {
        std::vector<Arg> objs;
        for (std::size_t i = 0; i < in.ops.size() && i < ob.size(); ++i)
            if (ob[i]) oblige_node(operand(fr, in.ops[i], cur, line), *ob[i], in.callee, objs, cur, line);
        for (std::size_t i = 0; i < objs.size(); ++i)
            for (std::size_t j = i + 1; j < objs.size(); ++j)
                mt.obligation_distinct(cur, objs[i], objs[j], "call of @" + in.callee, line);
    }

    // Instructions of g and of every function it calls directly, transitively (TranslateOptions::small_inline_insts);
    // an indirect call, an asm goto, a recursive tree or one over the limit counts as "large" (limit + 1).
    std::map<const ir::Function*, long> small_memo;
    long small_tree(const ir::Function& g) {
        const long big = static_cast<long>(opt.small_inline_insts) + 1;
        if (auto it = small_memo.find(&g); it != small_memo.end()) return it->second;
        small_memo[&g] = big;  // in progress: a cycle is large
        long n = 0;
        for (auto& bl : g.blocks) {
            for (auto& ins : bl.insts) {
                if (++n >= big) return big;
                if (ins.op == "callbr" || ins.op == "invoke" || (ins.op == "call" && !ins.is_asm && ins.callee.empty()))
                    return big;
                if (ins.op == "call" && !ins.callee.empty())
                    if (const auto* c = m.find(ins.callee); c && !c->blocks.empty()) {
                        n += small_tree(*c);
                        if (n >= big) return big;
                    }
            }
        }
        small_memo[&g] = n;
        return n;
    }

    void inline_call(Frame& fr, const ir::Inst& in, int& cur, int line) {
        const auto& n = in.callee;
        const auto* callee = m.find(n);
        if (std::ranges::find(stack, n) != stack.end())
            throw Unenc{"UNENCODED: recursive call @" + n};
        // Depth: user frames (stack minus the library-model frames on it) are bounded by inline_depth; a model callee
        // is exempt from that bound and bounded only by inline_depth + model_inline_depth frames in all (models are
        // inlined exactly, so the exemption adds no assumption). Past either bound the call is UNENCODED, as before.
        {
            const int frames = static_cast<int>(stack.size());
            const bool model_call = callee && callee->is_model && opt.model_inline_depth > 0;
            if (model_call) {
                const int cap = opt.inline_depth + opt.model_inline_depth;
                if (frames > cap)
                    throw Unenc{"UNENCODED: call depth > " + std::to_string(cap) + " (@" + n + ") (library model frames)"};
            } else {
                const int user = opt.model_inline_depth > 0 ? frames - static_cast<int>(model_stack.size()) : frames;
                // a small call tree (the kernel's one-line wrappers: kasan_check_*, kobject_name, refcount_read ->
                // atomic_read -> ...) is inlined past inline_depth when its whole tree has at most
                // small_inline_insts instructions; the total cap still holds
                const bool small = opt.small_inline_insts > 0 && callee && user > opt.inline_depth &&
                                   frames <= opt.inline_depth + opt.model_inline_depth &&
                                   small_tree(*callee) <= opt.small_inline_insts;
                if (user > opt.inline_depth && !small)
                    throw Unenc{"UNENCODED: call depth > " + std::to_string(opt.inline_depth) + " (@" + n + ")"};
            }
        }
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
        // a variadic model (printk-like specs) ignores the arguments after its fixed ones; they were
        // evaluated above (undef and poison uses are still checked)
        if (args.size() > callee->params.size() && callee->variadic && callee->is_model) {
            args.resize(callee->params.size());
            raw_arg.resize(callee->params.size());
        }
        if (args.size() != callee->params.size()) throw Unenc{"UNENCODED: call @" + n + " arity"};
        if (callee->is_model && !callee->blocks.empty()) {
            // A library contract (its body starts with __prism_contract) is
            // bound by name: a declaration of another signature is another
            // function, never answered by the contract.
            bool contract = false;
            for (auto& ci : callee->blocks.front().insts)
                if (ci.op == "call" && !ci.callee.starts_with("llvm.dbg")) {
                    contract = ci.callee == "__prism_contract";
                    break;
                }
            for (std::size_t i = 0; contract && i < in.ops.size(); ++i) {
                const auto& a = in.ops[i].ty;
                const auto& pt = callee->params[i].ty;
                if (a.kind != pt.kind || (a.kind == ir::Type::Int && a.bits != pt.bits))
                    throw Unenc{"UNENCODED: call @" + n + " (its declaration does not match the library contract's "
                                "signature)"};
            }
            if (contract && (in.ty.kind != callee->ret.kind ||
                             (in.ty.kind == ir::Type::Int && in.ty.bits != callee->ret.bits)))
                throw Unenc{"UNENCODED: call @" + n + " (its return type does not match the library contract's)"};
        }
        Frame cf;
        for (std::size_t i = 0; i < args.size(); ++i)
            if (raw_arg.at(i) && !args.at(i).is_const) {
                cf.shadow[args.at(i).var] = fr.shadow[args.at(i).var];
                cf.raw.insert(args.at(i).var);
            }
        cf.f = callee;
        cf.prefix = n + "#" + std::to_string(uniq++) + ".";
        cf.serial = ++frame_serial;
        for (std::size_t i = 0; i < args.size(); ++i) {
            auto w = vwidth(callee->params[i].ty, "call @" + n + " parameter");
            args.at(i).width = w;
            if (auto bt = byval_type(callee->params[i].attrs)) {
                // by-value aggregate: the callee gets its own copy
                auto sz = mt.layout().alloc_size(*bt);
                auto copy = mt.alloc(cur, Arg::c(64, sz), MemKind::Stack, 0, callee->params[i].name, line);
                mt.memcpy_(cur, copy, args.at(i), Arg::c(64, sz), true, line);
                cf.allocas.push_back(copy);
                args.at(i) = copy;
            }
            cf.env[callee->params[i].name] = args.at(i);
        }
        int cont = newblock(fr.prefix + "call." + n + "." + std::to_string(uniq++));
        cf.ret_block = cont;
        if (ret_pair) {
            // a scalar pair (e.g. std::pair<iterator, bool>): two return variables
            if (auto it = fr.pairs.find(in.result); !in.result.empty() && it != fr.pairs.end())
                cf.ret_pair = it->second;
            else
            {
                cf.ret_pair = std::pair{newvar(tmpname("ret"), ret_pair->first), newvar(tmpname("ret"), ret_pair->second)};
                mark_fp_pair(callee->ret, cf.ret_pair->first, cf.ret_pair->second);
            }
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
                if (attrs.at(e) == '(') ++depth;
                if (attrs.at(e) == ')') --depth;
                if (depth > 0) ++e;
            }
            return ir::parse_type(attrs.substr(a, e - a));
        }
        return std::nullopt;
    }

    bool is_ptrtoint(Frame& fr, const ir::Operand& o) {
        if (opt.addr_values && o.v.kind == ir::Value::ConstExpr && o.v.ce_op == "ptrtoint") return true;
        if (o.v.kind != ir::Value::Local) return false;
        if (fr.ptr_ints.count(o.v.name)) return true;
        auto it = fr.defs.find(o.v.name);
        return it != fr.defs.end() && it->second != nullptr && it->second->op == "ptrtoint";
    }

    // alloca/load/store/getelementptr/ptrtoint/inttoptr/bitcast; false = not a memory instruction
    // Field k of the scalar pair `ty` stored at p: fn(k, pointer, alignment).
    template <typename Fn>
    void pair_fields(int cur, const ir::Type& ty, Arg p, unsigned align, int line, Fn fn, int q = -1) {
        const auto& lay = mt.layout();
        const uint64_t off1 = lay.field_offset(ty, 1);
        const unsigned a0 = align ? align : lay.align(ty);
        // the second field's alignment: what the struct's alignment and the
        // field offset guarantee
        unsigned a1 = a0;
        while (a1 > 1 && off1 % a1 != 0) a1 /= 2;
        fn(0, p, a0);
        if (q < 0) q = newvar(tmpname("pair.f1"), ptr_w());
        mt.gep(cur, q, p, ty, {Arg::c(32, 0), Arg::c(32, 1)}, true, line, 1);
        fn(1, Arg::v(q, ptr_w()), a1);
    }

    bool mem_inst(Frame& fr, const ir::Inst& in, int cur, int line) {
        const auto& op = in.op;
        if (op == "alloca") {
            int dst = result_var(fr, in);
            std::optional<Arg> count, src;
            if (!in.ops.empty()) {
                count = operand(fr, in.ops[0], cur, line);
                if (count.has_value()) count->width = int_width(in.ops[0].ty, "alloca count");
                if (in.ops[0].v.kind == ir::Value::Local)
                    if (auto it = fr.defs.find(in.ops[0].v.name);
                        it != fr.defs.end() && it->second != nullptr &&
                        (it->second->op == "zext" || it->second->op == "sext") && !it->second->ops.empty() &&
                        it->second->ops[0].ty.kind == ir::Type::Int) {
                        src = operand(fr, it->second->ops[0], cur, line);
                        src->width = int_width(it->second->ops[0].ty);
                    }
            }
            mt.alloca_(cur, dst, in.ety, in.align, count, src, in.result, line);
            if (!count && fr.f && !fr.f->blocks.empty() && cur == head[fr.prefix + fr.f->blocks.front().name])
                fr.allocas.push_back(Arg::v(dst, ptr_w()));
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
            }, fr.pairq.at(in.result));
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

            // C alignment of the access, not LLVM's: since LLVM 23 clang
            // annotates `*(int *)(buf + 2)` with the alignment it can prove
            // for that address (align 2) instead of int's, which hid the
            // C17 6.3.2.3p7 / 6.5 misaligned access (testdata/pir
            // mem_ptr.c:misaligned_bad). Below the type's ABI alignment the
            // IR value is trusted only for a packed struct member.
            auto c_align = [&](const ir::Value& pv, const ir::Type& ty, unsigned ir_al) -> unsigned {
                if (ty.kind != ir::Type::Int && ty.kind != ir::Type::Float && ty.kind != ir::Type::Ptr) return ir_al;
                unsigned nat = 0;
                try {
                    nat = mt.layout().align(ty);
                } catch (const Unenc&) {
                    return ir_al;
                }
                if (ir_al >= nat) return ir_al;
                auto packed_in = [&](const ir::Type& t0) {
                    const ir::Type* t = &t0;
                    for (int d = 0; d < 16; ++d) {
                        const auto& rt = mt.layout().resolve(*t);
                        if (rt.kind == ir::Type::Struct) {
                            if (rt.text.starts_with("<{")) return true;
                            for (auto& e : rt.elems) {
                                const auto& re = mt.layout().resolve(e);
                                if (re.kind == ir::Type::Struct && re.text.starts_with("<{")) return true;
                            }
                            return false;
                        }
                        if (rt.kind != ir::Type::Array || rt.elems.empty()) return false;
                        t = &rt.elems.at(0);
                    }
                    return false;
                };
                // Only byte/scalar pointer arithmetic lowers the alignment
                // below the C type's: a struct passed by value is loaded as
                // its ABI coercion type (i64 at the struct's align 4), which
                // is no C access and keeps the IR value.
                auto scalar = [](const ir::Type& t) {
                    return t.kind == ir::Type::Int || t.kind == ir::Type::Float || t.kind == ir::Type::Ptr;
                };
                auto moves = [](const auto& o) { return o.v.kind != ir::Value::Int || o.v.bits != 0; };
                bool arith = false;
                const ir::Value* v = &pv;
                for (int depth = 0; depth < 32 && v->kind == ir::Value::Local; ++depth) {
                    auto it = fr.defs.find(v->name);
                    if (it == fr.defs.end() || it->second->op != "getelementptr" || it->second->ops.size() < 2) break;
                    const auto* d = it->second;
                    try {
                        if (packed_in(d->ety)) return ir_al;
                        const auto& rt = mt.layout().resolve(d->ety);
                        if (scalar(rt) && moves(d->ops[1])) arith = true;
                        if (rt.kind == ir::Type::Array && !rt.elems.empty() &&
                            scalar(mt.layout().resolve(rt.elems.at(0))) && moves(d->ops.back()))
                            arith = true;
                    } catch (const Unenc&) {
                        return ir_al;
                    }
                    v = &d->ops[0].v;
                }
                return arith ? nat : ir_al;
            };
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
            int sh = mt.load(cur, dst, in.ty, p, c_align(in.ops[0].v, in.ty, in.align), line, !agg);
            if (agg && dst >= 0) {
                fr.shadow[dst] = Arg::v(sh, out.vars[static_cast<std::size_t>(sh)].width);
                fr.raw.insert(dst);
            }
            return true;
        }
        if (op == "store") {
            if (in.ops.size() != 2 || in.ops[1].ty.kind != ir::Type::Ptr || in.ops[1].ty.text != "ptr")
                throw Unenc{"UNENCODED: store"};

            // (as for load) C alignment of the access, not LLVM's: since LLVM 23 clang
            // annotates `*(int *)(buf + 2)` with the alignment it can prove
            // for that address (align 2) instead of int's, which hid the
            // C17 6.3.2.3p7 / 6.5 misaligned access (testdata/pir
            // mem_ptr.c:misaligned_bad). Below the type's ABI alignment the
            // IR value is trusted only for a packed struct member.
            auto c_align = [&](const ir::Value& pv, const ir::Type& ty, unsigned ir_al) -> unsigned {
                if (ty.kind != ir::Type::Int && ty.kind != ir::Type::Float && ty.kind != ir::Type::Ptr) return ir_al;
                unsigned nat = 0;
                try {
                    nat = mt.layout().align(ty);
                } catch (const Unenc&) {
                    return ir_al;
                }
                if (ir_al >= nat) return ir_al;
                auto packed_in = [&](const ir::Type& t0) {
                    const ir::Type* t = &t0;
                    for (int d = 0; d < 16; ++d) {
                        const auto& rt = mt.layout().resolve(*t);
                        if (rt.kind == ir::Type::Struct) {
                            if (rt.text.starts_with("<{")) return true;
                            for (auto& e : rt.elems) {
                                const auto& re = mt.layout().resolve(e);
                                if (re.kind == ir::Type::Struct && re.text.starts_with("<{")) return true;
                            }
                            return false;
                        }
                        if (rt.kind != ir::Type::Array || rt.elems.empty()) return false;
                        t = &rt.elems.at(0);
                    }
                    return false;
                };
                // Only byte/scalar pointer arithmetic lowers the alignment
                // below the C type's: a struct passed by value is loaded as
                // its ABI coercion type (i64 at the struct's align 4), which
                // is no C access and keeps the IR value.
                auto scalar = [](const ir::Type& t) {
                    return t.kind == ir::Type::Int || t.kind == ir::Type::Float || t.kind == ir::Type::Ptr;
                };
                auto moves = [](const auto& o) { return o.v.kind != ir::Value::Int || o.v.bits != 0; };
                bool arith = false;
                const ir::Value* v = &pv;
                for (int depth = 0; depth < 32 && v->kind == ir::Value::Local; ++depth) {
                    auto it = fr.defs.find(v->name);
                    if (it == fr.defs.end() || it->second->op != "getelementptr" || it->second->ops.size() < 2) break;
                    const auto* d = it->second;
                    try {
                        if (packed_in(d->ety)) return ir_al;
                        const auto& rt = mt.layout().resolve(d->ety);
                        if (scalar(rt) && moves(d->ops[1])) arith = true;
                        if (rt.kind == ir::Type::Array && !rt.elems.empty() &&
                            scalar(mt.layout().resolve(rt.elems.at(0))) && moves(d->ops.back()))
                            arith = true;
                    } catch (const Unenc&) {
                        return ir_al;
                    }
                    v = &d->ops[0].v;
                }
                return arith ? nat : ir_al;
            };
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
            mt.store(cur, p, v, init, vo.ty, c_align(in.ops[1].v, vo.ty, in.align), line);
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
            // gep nuw (LLVM 19+): poison if any index, read as unsigned, makes the
            // offset arithmetic wrap. A negative index (variable or constant; a
            // struct field index is never negative) always wraps, so it is checked
            // here (Lean: PrismRefine.nuwChecks); the rest is bounded by the
            // inbounds/object check.
            // nusw is implied by inbounds (checked by MemTr::gep); without
            // inbounds it is dropped (conservative: the result is not trusted).
            if (has_flag(in, "nuw"))
                for (std::size_t k = 1; k < in.ops.size(); ++k) {
                    if (in.ops[k].v.kind != ir::Value::Local && in.ops[k].v.kind != ir::Value::Int) continue;
                    unsigned iw = int_width(in.ops[k].ty);
                    Arg ix = operand(fr, in.ops[k], cur, line);
                    ix.width = iw;
                    check(cur, p2(cur, Op::Slt, ix, Arg::c(iw, 0)), "nuw", "UB-POISON",
                          "getelementptr nuw with a negative index (poison)", line);
                }
            mt.gep(cur, result_var(fr, in), base, in.ety, idx, has_flag(in, "inbounds"), line, use, base_last);
            return true;
        }
        if (op == "ptrtoint") {
            // The pointer value is carried as its packed integer representation, so
            // ptrtoint is a plain copy; an inttoptr of this value round-trips exactly,
            // and pointer differences are still checked in the sub path.
            if (in.ty.kind != ir::Type::Int || in.ty.bits != ptr_w())
                throw Unenc{"UNENCODED: ptrtoint (address values are not modelled)"};
            Arg p = operand(fr, in.ops[0], cur, line);
            emit_assign(cur, result_var(fr, in), Op::Copy, {p});
            return true;
        }
        if (op == "inttoptr") {
            const auto& o = in.ops[0];
            if (o.ty.kind != ir::Type::Int || o.ty.bits != ptr_w())
                throw Unenc{"UNENCODED: inttoptr (operand is not pointer width)"};
            if (is_ptrtoint(fr, o)) {
                emit_assign(cur, result_var(fr, in), Op::Copy, {operand(fr, o, cur, line)});
                return true;
            }
            Arg iv = operand(fr, o, cur, line);
            if (iv.is_const && iv.bits == 0) {
                emit_assign(cur, result_var(fr, in), Op::Copy, {Arg::c(ptr_w(), 0)});
                return true;
            }
            note_assumption("inttoptr-opaque: integer-to-pointer cast of a value that is not a ptrtoint result (line " +
                            std::to_string(line) + ")");
            if (iv.is_const) {
                emit_assign(cur, result_var(fr, in), Op::Copy, {Arg::c(ptr_w(), iv.bits)});
                return true;
            }
            emit_assign(cur, result_var(fr, in), Op::Copy, {havoc(cur, ptr_w(), false, true)});
            return true;
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
            // Thread-scheduler model classes (models/cxx/bits/prism_sched.h)
            // onto the taxonomy: a wait/lock no schedule can satisfy is a
            // deadlock (CWE-833, property nodeadlock); a std::thread destroyed
            // or move-assigned while joinable calls std::terminate.
            if (*cls == "CXX-DEADLOCK") cls = std::string("CONC-DEADLOCK");
            else if (*cls == "CXX-THREAD-JOINABLE") cls = std::string("CXX-TERMINATE");
            check(cur, assign(cur, Op::Eq, 1, {ok, Arg::c(ok.width, 0)}, "c"), "model", *cls, *msg, line);
            return;
        }
        // __prism_incomplete(msg): a library model's path that PRISM does not
        // follow (e.g. regex syntax the matcher does not implement). Reaching
        // it is a soft stop (translate_ctl.inc): NEEDS-HARNESS when reachable,
        // never FAILED, never PROVED; an unreachable one is no obstacle.
        if (n == "__prism_incomplete") {
            auto msg = in.ops.empty() ? std::nullopt : mt.const_string(in.ops.at(0).v);
            if (!msg) throw Unenc{"UNENCODED: __prism_incomplete without a literal message"};
            soft_stop(cur, "model incomplete: " + *msg, line);
            cur = newblock("incomplete.after");  // unreachable
            return;
        }
        if (n == "__prism_assumption") {
            // a kernel API spec records the assumption it introduces (docs/KERNEL_MODELS.md)
            auto txt = in.ops.empty() ? std::nullopt : mt.const_string(in.ops.at(0).v);
            if (!txt) throw Unenc{"UNENCODED: __prism_assumption without a literal text"};
            note_assumption(*txt);
            return;
        }
        if (n == "__prism_assume") {
            Arg c = arg(0);
            assume(cur, assign(cur, Op::Ne, 1, {c, Arg::c(c.width, 0)}, "c"));
            return;
        }
        // __prism_contract(id): this call of an external function is
        // answered by a library contract (models/kernel/, docs/CONTRACTS.md):
        // its requires are checked, its ensures assumed. The contract is an
        // assumption of the caller's proof (PROVED-ASSUMING, never PROVED).
        if (n == "__prism_contract") {
            auto id = in.ops.empty() ? std::nullopt : mt.const_string(in.ops.at(0).v);
            if (!id) throw Unenc{"UNENCODED: __prism_contract without a literal id"};
            // A model #included into the unit (its verification harness,
            // testdata/contracts/kmodels/) is the code under check: nothing
            // is assumed about it.
            if (model_stack.empty()) return;
            auto text = "callee behaves as the library contract " + *id;
            if (std::ranges::find(out.assumptions, text) == out.assumptions.end()) out.assumptions.push_back(text);
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
        // ghost lock state of the kernel lock specs (translate_lock.inc)
        if (n == "__prism_lock_acquire") {
            lock_take(cur, arg(0), arg(1), line);
            return;
        }
        if (n == "__prism_lock_release") {
            lock_drop(cur, arg(0), arg(1), line);
            return;
        }
        if (n == "__prism_sleep_point") {
            sleep_point(cur, line);
            return;
        }
        if (n == "__prism_sleep_if") {
            sleep_if(cur, arg(0), line);
            return;
        }
        if (n == "__prism_atomic_add") {
            atomic_add(cur, arg(0), line);
            return;
        }
        if (n == "__prism_rcu_add") {
            rcu_add(cur, arg(0), line);
            return;
        }
        if (n == "__prism_lock_entry" || n == "__prism_lock_exit") {
            // contract markers: read only in the analysed function (translate_lock.inc)
            if (lock_root(fr)) lock_declare(cur, n == "__prism_lock_entry", arg(0), n == "__prism_lock_entry" ? arg(1) : Arg::c(1, 0), line);
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
        for (const auto& lp : lpad_phis) {
            // one incoming per throw edge, valued by the invoke it passed through
            PPhi p;
            p.block = lp.block;
            p.dst = lp.dst;
            for (const auto& e : eh_edges)
                if (e.frame == lp.frame && e.lpad == lp.lpad)
                    for (const auto& [pred, v] : lp.in)
                        if (pred == e.invoke_block) {
                            p.in.push_back(PPhiIn{"", e.pred, v, 0});
                            break;
                        }
            pphis.push_back(p);
        }
        for (const auto& p : pphis) {
            Phi phi;
            phi.dst = p.dst;
            for (const auto& edge : p.in) {
                const auto& qname = edge.qname;
                int pred = edge.idx;
                const Arg a = edge.a;
                const int kind = edge.kind;
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
#include "translate_kernel.inc"
#include "translate_asm.inc"
#include "translate_lock.inc"

bool Tr::format_call(Frame& fr, const ir::Inst& in, int& cur, int line) {
    const auto& n = in.callee;
    if (m.find(n) || !pirmem::is_format_function(n)) return false;
    std::vector<ir::Type> tys;
    for (auto& o : in.ops) tys.push_back(o.ty);
    std::optional<std::string> fmt;
    auto fi = n == "printf" ? 0u : n == "snprintf" ? 2u : 1u;
    if (fi < in.ops.size()) fmt = mt.const_string(in.ops[fi].v);
    std::vector<std::optional<uint64_t>> lits;
    // Int: two's complement bits; Fp: IEEE bits (libc_format.cpp tells them apart by the IR type)
    for (auto& o : in.ops)
        lits.push_back(o.v.kind == ir::Value::Int || o.v.kind == ir::Value::Fp ? std::optional<uint64_t>(o.v.bits) : std::nullopt);
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
        Arg a = args.at(i);
        a.width = ptr_w();
        check(cur, assign(cur, Op::Eq, 1, {assign(cur, Op::LShr, ptr_w(), {a, Arg::c(ptr_w(), obj_shift())}), Arg::c(ptr_w(), 0)}),
              "null", "PTR-NULL-DEREF", n + " %s argument is a null pointer", line);
        inline_named(fr, "strlen", {in.ops[i]}, cur, line);
    }
    if (plan.buf_arg >= 0 && plan.exact_double_arg >= 0 && m.find("__prism_fmtv")) {
        // exact output bytes (coverage item 1): every variadic argument goes
        // to a typed slot of __prism_fmtv (8 each of i32, i64, double,
        // x86_fp80, ptr), which renders the whole format with prism_fpfmt.
        const ir::Type ti32{ir::Type::Int, 32, "i32", {}}, ti64{ir::Type::Int, 64, "i64", {}},
            tdbl{ir::Type::Float, 0, "double", {}}, tld{ir::Type::Float, 0, "x86_fp80", {}}, tptr{ir::Type::Ptr, 0, "ptr", {}};
        auto zero = [](const ir::Type& t) {
            ir::Operand o;
            o.ty = t;
            o.v.kind = t.kind == ir::Type::Ptr ? ir::Value::Null : t.kind == ir::Type::Float ? ir::Value::Fp : ir::Value::Int;
            o.v.bits = 0;
            return o;
        };
        // Windows of 8 arguments (conversions starting in the first 6 of them,
        // so '*' width, '*' precision and the value always fit), chained
        // through the running length: no cap on the argument count.
        auto cls = [](const ir::Operand& o) {
            if (o.ty.kind == ir::Type::Int && o.ty.bits == 32) return 0;
            if (o.ty.kind == ir::Type::Int && o.ty.bits == 64) return 1;
            if (o.ty.kind == ir::Type::Float && o.ty.text == "double") return 2;
            if (o.ty.kind == ir::Type::Float && o.ty.text == "x86_fp80") return 3;
            if (o.ty.kind == ir::Type::Ptr && o.attrs.find("byval") != std::string::npos) return 6;
            if (o.ty.kind == ir::Type::Ptr) return 4;
            if (o.ty.kind == ir::Type::Int && o.ty.bits == 128) return 5;
            return -1;
        };
        bool fits = true;
        const std::size_t na = in.ops.size() - (fi + 1);
        for (std::size_t k = fi + 1; k < in.ops.size(); ++k) if (cls(in.ops[k]) < 0) fits = false;
        if (fits) {
            ir::Operand cap;
            if (plan.size_arg >= 0) cap = in.ops[static_cast<std::size_t>(plan.size_arg)];
            else {
                cap = zero(ti64);
                cap.v.bits = (~0ull) >> 2;  // sprintf: unbounded (the memory model checks each byte)
            }
            ir::Operand pos = zero(ti32);
            const std::string base = (in.result.empty() ? std::string("prism.fmtv") : in.result) + ".w" + std::to_string(fmt_seq++) + ".";
            for (std::size_t ab = 0;; ab += 6) {
                const bool last = ab + 6 >= na;
                ir::Inst call;
                call.op = "call";
                call.callee = "__prism_fmtv";
                call.result = last && !in.result.empty() ? in.result : base + std::to_string(ab);
                call.ty = ti32;
                ir::Operand oab = zero(ti32), oae = zero(ti32);
                oab.v.bits = ab;
                oae.v.bits = last ? 0x7fffffffu : ab + 6;
                ir::Operand ocl = zero(ti32);
                std::vector<ir::Operand> sv[6];
                for (std::size_t k = ab; k < std::min(na, ab + 8); ++k) {
                    const int c = cls(in.ops[fi + 1 + k]);
                    ocl.v.bits |= static_cast<uint64_t>(c) << (3 * (k - ab));
                    // byval structs travel in the pointer slots (the model reads their eightbytes)
                    ir::Operand o = in.ops[fi + 1 + k];
                    if (c == 6) o.attrs.clear();
                    sv[c == 6 ? 4 : c].push_back(o);
                }
                call.ops = {in.ops[static_cast<std::size_t>(plan.buf_arg)], cap, in.ops[fi], oab, oae, pos, ocl};
                const ir::Type ti128{ir::Type::Int, 128, "i128", {}};
                const ir::Type* tys[6] = {&ti32, &ti64, &tdbl, &tld, &tptr, &ti128};
                for (int c = 0; c < 6; ++c)
                    for (std::size_t k = 0; k < 8u; ++k) call.ops.push_back(k < sv[c].size() ? sv[c][k] : zero(*tys[c]));
                inline_call(fr, call, cur, line);
                if (last) return true;
                pos = ir::Operand{};
                pos.ty = ti32;
                pos.v.kind = ir::Value::Local;
                pos.v.name = call.result;
            }
        }
    }
    if (plan.buf_arg >= 0) {
        Arg buf = args.at(static_cast<std::size_t>(plan.buf_arg));
        buf.width = ptr_w();
        Arg len = havoc(cur, ptr_w(), false, false);
        if (plan.size_arg >= 0) {
            Arg sz = args.at(static_cast<std::size_t>(plan.size_arg));
            if (sz.width < ptr_w()) sz = assign(cur, Op::ZExt, ptr_w(), {sz});
            // snprintf(buf, 0, ...) writes nothing; otherwise at most size-1 characters and a NUL
            int wb = newblock("snprintf.write"), done = newblock("snprintf.done");
            Term t;
            t.kind = Term::Br;
            t.cond = assign(cur, Op::Ne, 1, {sz, Arg::c(ptr_w(), 0)});
            t.t = wb;
            t.f = done;
            out.blocks[static_cast<std::size_t>(cur)].term = t;
            cur = wb;
            assume(cur, assign(cur, Op::Ult, 1, {len, sz}));
            mt.write_havoc(cur, buf, len, line);
            int end = newvar(tmpname("end"), ptr_w());
            mt.gep(cur, end, buf, ir::Type{ir::Type::Int, 8, "i8", {}}, {len}, true, line);
            mt.store(cur, Arg::v(end, ptr_w()), Arg::c(8, 0), Arg::c(1, 1), ir::Type{ir::Type::Int, 8, "i8", {}}, 1, line);
            Term j;
            j.kind = Term::Jmp;
            j.t = done;
            out.blocks[static_cast<std::size_t>(cur)].term = j;
            cur = done;
        } else {
            assume(cur, assign(cur, Op::Ule, 1, {len, Arg::c(ptr_w(), plan.max_len)}));
            mt.write_havoc(cur, buf, len, line);
            int end = newvar(tmpname("end"), ptr_w());
            mt.gep(cur, end, buf, ir::Type{ir::Type::Int, 8, "i8", {}}, {len}, true, line);
            mt.store(cur, Arg::v(end, ptr_w()), Arg::c(8, 0), Arg::c(1, 1), ir::Type{ir::Type::Int, 8, "i8", {}}, 1, line);
        }
    }
    if (int dst = in.result.empty() ? -1 : result_var(fr, in); dst >= 0)
        havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
    return true;
}

// Pointer parameter bound to a contract object (Law 6 relaxation, docs/PIR.md).
void bind_contract(Tr& tr, const ir::Function& f, const ir::Param& p, const PtrContract& c, Frame& top) {
    uint64_t esz = pirmem::contract_elem_bytes_deep(tr.mt.layout(), tr.m, f, p, c);
    if (!esz)
        throw Unenc{"UNENCODED: element size of pointer parameter " + p.name + " unknown (contract " + c.text + ")"};
    Arg size;
    std::string count_text;
    if (c.count >= 0) {
        if (static_cast<uint64_t>(c.count) > max_obj_size() / esz) throw Unenc{"UNENCODED: contract object too large"};
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
        tr.assume(-1, tr.assign(-1, Op::Ult, 1, {nn, Arg::c(64, max_obj_size() / esz)}));
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

// Allocates the objects of an automatic pointer harness (harness.cpp) and
// stores the nested pointers: returns the pointer to `n`'s object.
Arg alloc_harness_node(Tr& tr, const pirmem::HarnessNode& n) {
    auto obj = tr.mt.alloc(-1, Arg::c(64, n.bytes), MemKind::Extern, 2, "*" + n.name + " (auto harness)", 0);
    const auto ptr_ty = ir::parse_type("ptr");
    std::set<uint64_t> bound;  // byte offsets (from the object start) that got an object of their own
    for (const auto& [off, child] : n.nested) {
        Arg cp = alloc_harness_node(tr, *child);
        int slot = tr.newvar(tr.tmpname("hslot"), ptr_w());
        tr.mt.gep(-1, slot, obj, ir::parse_type("i8"), {Arg::c(64, n.member_off + off)}, true, 0);
        tr.mt.store(-1, Arg::v(slot, ptr_w()), cp, Arg::c(1, 1), ptr_ty, 1, 0);
        bound.insert(n.member_off + off);
    }
    // --pir-field-invariants: value invariants of this struct's fields (the first element only), one named assumption
    // per row used; a field that already has its own harness object is left as it is
    if (n.type_known && n.type.kind == ir::Type::Struct && !tr.opt.field_invariants.empty()) {
        const uint64_t obj_bytes = n.bytes;
        for (const auto& fi : tr.opt.field_invariants) {
            if (fi.struct_type != n.type.text || fi.offset + fi.width > obj_bytes || bound.count(fi.offset)) continue;
            int slot = tr.newvar(tr.tmpname("hfi"), ptr_w());
            tr.mt.gep(-1, slot, obj, ir::parse_type("i8"), {Arg::c(64, fi.offset)}, true, 0);
            const Arg at = Arg::v(slot, ptr_w());
            std::string what;
            if (fi.kind == "valid") {
                auto o = tr.mt.alloc(-1, Arg::c(64, static_cast<uint64_t>(fi.a)), MemKind::Extern, 2,
                                     "*" + n.name + "->@" + std::to_string(fi.offset) + " (field invariant)", 0);
                tr.mt.store(-1, at, o, Arg::c(1, 1), ptr_ty, 1, 0);
                what = "points to a valid object of " + std::to_string(fi.a) + " bytes (arbitrary contents)";
            } else {
                const unsigned bits = fi.kind == "nonnull" ? ptr_w() : fi.width * 8;
                const auto ty = fi.kind == "nonnull" ? ptr_ty : ir::parse_type("i" + std::to_string(bits));
                int v = tr.newvar(tr.tmpname("hfiv"), bits);
                tr.mt.load(-1, v, ty, at, 1, 0, false);
                const Arg val = Arg::v(v, bits);
                if (fi.kind == "nonnull") {
                    tr.assume(-1, tr.assign(-1, Op::Ne, 1, {val, Arg::c(bits, 0)}, "fi"));
                    what = "is not NULL";
                } else {
                    const uint64_t m = bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
                    Arg lo = Arg::c(bits, static_cast<uint64_t>(fi.a) & m), hi = Arg::c(bits, static_cast<uint64_t>(fi.b) & m);
                    tr.assume(-1, tr.assign(-1, Op::Xor, 1, {tr.assign(-1, Op::Slt, 1, {val, lo}, "fi"), Arg::c(1, 1)}, "fi"));
                    tr.assume(-1, tr.assign(-1, Op::Xor, 1, {tr.assign(-1, Op::Slt, 1, {hi, val}, "fi"), Arg::c(1, 1)}, "fi"));
                    what = "is in [" + std::to_string(fi.a) + ", " + std::to_string(fi.b) + "] (signed, " +
                           std::to_string(bits) + " bits)";
                }
            }
            const std::string a = "field-invariant: " + n.name + "->@" + std::to_string(fi.offset) + " (" + fi.struct_type +
                                  " at byte " + std::to_string(fi.offset) + ") " + what + " (--pir-field-invariants; scope " +
                                  (fi.scope.empty() ? std::string("global") : fi.scope) + "; status " + fi.status + "; source " +
                                  fi.source + "; not checked here)";
            if (std::find(tr.out.assumptions.begin(), tr.out.assumptions.end(), a) == tr.out.assumptions.end())
                tr.out.assumptions.push_back(a);
        }
    }
    // function-pointer fields known to the ops tables (--pir-ops-auto): one of the targets, chosen freely
    for (const auto& fp : n.fp_fields) {
        Arg val = tr.fn_addr(fp.targets.front());
        for (std::size_t i = 1; i < fp.targets.size(); ++i) {
            Arg pick = tr.havoc(-1, 1, false, false);
            val = tr.assign(-1, Op::Select, ptr_w(), {pick, tr.fn_addr(fp.targets[i]), val});
        }
        int slot = tr.newvar(tr.tmpname("hfp"), ptr_w());
        tr.mt.gep(-1, slot, obj, ir::parse_type("i8"), {Arg::c(64, n.member_off + fp.offset)}, true, 0);
        tr.mt.store(-1, Arg::v(slot, ptr_w()), val, Arg::c(1, 1), ptr_ty, 1, 0);
    }
    if (n.member_off) {  // container_of: the parameter points into the containing object
        int mp = tr.newvar(tr.tmpname("hmember"), ptr_w());
        tr.mt.gep(-1, mp, obj, ir::parse_type("i8"), {Arg::c(64, n.member_off)}, true, 0);
        return Arg::v(mp, ptr_w());
    }
    return obj;
}

// Pointer parameter bound to an object built from the IR types (--pir-harness).
// Returns false (and `why`) when no object can be built.
bool bind_auto_harness(Tr& tr, const ir::Function& f, const ir::Param& p, Frame& top, std::string* why) {
    pirmem::HarnessOptions ho;
    ho.depth = tr.opt.auto_harness.depth;
    ho.elems = tr.opt.auto_harness.elems;
    ho.unknown_bytes = tr.opt.auto_harness.unknown_bytes;
    ho.max_objects = tr.opt.auto_harness.max_objects;
    ho.container_of = tr.opt.container_of;
    if (tr.opt.ops_auto) {
        ho.ops = &tr.opt.ops_table;
        ho.ops_module = &tr.m;
    }
    auto plan = pirmem::plan_pointer_harness(tr.m, tr.mt.layout(), f, p, ho, why);
    if (!plan) return false;
    top.env[p.name] = alloc_harness_node(tr, *plan->root);
    tr.out.ptr_params.push_back(p.name);
    for (auto& a : plan->assumptions) tr.out.assumptions.push_back(a);
    return true;
}

}  // namespace

std::vector<OpsField> load_ops_table(const std::filesystem::path& file, std::string& error) {
    error.clear();
    std::ifstream in(file);
    if (!in) {
        error = "cannot read " + file.string();
        return {};
    }
    std::vector<OpsField> out;
    // (struct_type, field) -> entry in out; position of a field = its first-seen order per struct
    std::map<std::pair<std::string, std::string>, std::size_t> at;
    std::map<std::string, int> nfields;
    auto trim = [](const std::string& s) {
        std::string::size_type b = 0, e = s.size();
        while (b < e && s[b] == ' ') ++b;
        while (e > b && s[e - 1] == ' ') --e;
        return s.substr(b, e - b);
    };
    std::string line;
    for (int line_no = 1; std::getline(in, line); ++line_no) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cols;
        for (std::string::size_type start = 0;;) {
            const auto p = line.find('\t', start);
            cols.push_back(line.substr(start, p == std::string::npos ? std::string::npos : p - start));
            if (p == std::string::npos) break;
            start = p + 1;
        }
        if (cols.size() < 4 || cols.size() > 5) {
            error = "line " + std::to_string(line_no) + ": expected 4 or 5 tab-separated columns";
            return {};
        }
        OpsField f;
        f.struct_type = trim(cols[0]);
        f.field = trim(cols[1]);
        f.role = cols[3];
        for (std::string::size_type s = 0; s <= cols[2].size();) {
            const auto p = cols[2].find(',', s);
            std::string t = cols[2].substr(s, p == std::string::npos ? std::string::npos : p - s);
            t = trim(t);
            if (!t.empty()) f.targets.push_back(t);
            if (p == std::string::npos) break;
            s = p + 1;
        }
        if (f.targets.empty()) {
            error = "line " + std::to_string(line_no) + ": no targets";
            return {};
        }
        const auto key = std::make_pair(f.struct_type, f.field);
        if (auto it = at.find(key); it != at.end()) {  // a field listed twice: one entry
            auto& e = out[it->second];
            e.targets.insert(e.targets.end(), f.targets.begin(), f.targets.end());
            continue;
        }
        f.index = nfields[f.struct_type]++;
        if (cols.size() == 5 && !cols[4].empty()) {
            try {
                std::size_t used = 0;
                f.index = std::stoi(cols[4], &used);
                if (used != cols[4].size() || f.index < 0) throw std::invalid_argument(cols[4]);
                for (const auto& e : out) {
                    if (e.struct_type == f.struct_type && e.index == f.index) {
                        error = "line " + std::to_string(line_no) + ": duplicate field_index";
                        return {};
                    }
                }
            } catch (const std::exception&) {
                error = "line " + std::to_string(line_no) + ": bad field_index";
                return {};
            }
        }
        at.emplace(key, out.size());
        out.push_back(std::move(f));
    }
    if (in.bad()) {
        error = "cannot read " + file.string();
        return {};
    }
    return out;
}

Translation translate(const ir::Module& m, const ir::Function& f, const TranslateOptions& opt) {
    const PtrModel pm = ptr_model_of(m);
    PtrModelScope pms(pm);
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
        if (p.ty.kind == ir::Type::Ptr && !own_object(p.attrs) && !contract_of(p.name) && !opt.auto_harness.on) {
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
            if (const auto* pc = contract_of(p->name)) {
                bind_contract(tr, f, *p, *pc, top);
            } else {
                std::string why;
                if (!bind_auto_harness(tr, f, *p, top, &why))
                    throw Unenc{"UNENCODED: automatic pointer harness for parameter " + p->name + ": " + why};
            }
            ++contracts;
        }
        if (contracts > 1)
            tr.out.assumptions.push_back("pointer parameters point to distinct objects (\\separated)");
        // a scalar pair result is not returned (Tr::term): width 0
        if (f.ret.kind != ir::Type::Void && !tr.scalar_pair(f.ret)) {
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
        tr.out.ptr = pm;
        t.fn = std::move(tr.out);
        t.folded_used.assign(tr.folded_used.begin(), tr.folded_used.end());
        t.status.clear();
    } catch (const Unenc& u) {
        t.reason = u.reason;
    }
    return t;
}

}  // namespace prism::pir
