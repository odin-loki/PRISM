#pragma once

// PIR: the PRISM intermediate representation (roadmap Part 2, docs/PIR.md).
//
// Pipeline: Clang -> LLVM IR (-O0, optnone off) -> fixed normalisation passes
// (mem2reg, lowerswitch, loop-simplify, lcssa, instnamer) -> textual IR
// parser (ir::) -> PIR (typed SSA over bitvectors with explicit property
// checks inserted by PRISM, Law 8 polarity) -> Z3 bitvector encoder
// (bounded unrolling + unwinding assertion + k-induction).
//
// Everything the translator does not model becomes an explicit
// "UNENCODED: <construct>" NEEDS-HARNESS verdict (roadmap 2.1); pointer
// parameters are NEEDS-HARNESS (Law 6). Nothing is dropped silently.

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism::pir {

// ---------------------------------------------------------------------------
// LLVM IR subset (textual, as printed by opt -S)
// ---------------------------------------------------------------------------
namespace ir {

struct Type {
    enum Kind { Int, Ptr, Void, Float, Label, Metadata, Struct, Array, Vector, Other };
    Kind kind = Other;
    unsigned bits = 0;         // Int width
    std::string text;          // as written ("i32", "{ i32, i1 }", "double")
    std::vector<Type> elems;   // Struct / Array / Vector element types
    bool is_int() const { return kind == Int; }
};

struct Value {
    enum Kind { Local, Global, Int, Undef, Poison, Null, Zero, Other };
    Kind kind = Other;
    std::string name;          // Local/Global name without sigil
    uint64_t bits = 0;         // Int: two's complement bits (masked by user)
    bool negative = false;     // Int literal was written negative
    std::string text;          // as written (constant expressions etc.)
};

struct Operand {
    Type ty;
    Value v;
    std::string attrs;         // call-site parameter attributes (signext ...)
};

struct Inst {
    std::string result;        // "%x" -> "x"; empty when no result
    std::string op;            // opcode ("add", "icmp", "call", "br", ...)
    Type ty;                   // result type (or first operand type)
    std::vector<Operand> ops;
    std::vector<std::string> flags;  // nsw nuw exact disjoint nneg samesign
    std::string pred;          // icmp predicate
    std::string callee;        // call: "@name" without '@' (empty: indirect)
    std::vector<std::pair<Operand, std::string>> incoming;  // phi
    std::vector<std::string> targets;  // br: [label] or [true, false]; switch: [default]
    std::vector<std::pair<Operand, std::string>> cases;     // switch
    std::vector<unsigned> indices;     // extractvalue
    std::string dbg;           // "!13" (DILocation ref) or empty
    std::string text;          // original text (messages)
    bool parsed = true;        // false: opcode known, operands not modelled
};

struct Block {
    std::string name;
    std::vector<Inst> insts;
};

struct Param {
    Type ty;
    std::string name;
    std::string attrs;         // signext / zeroext / noundef ...
};

struct Function {
    std::string name;          // IR symbol (mangled for C++)
    Type ret;
    std::string ret_attrs;
    std::vector<Param> params;
    std::vector<Block> blocks;
    std::string dbg;           // "!10" (DISubprogram ref)
    std::string parse_error;   // non-empty: body not parsed
    bool is_declaration = false;
};

struct DILoc {
    int line = 0;
    int col = 0;
};

struct DISub {
    std::string name;          // source name
    std::string linkage;
    int line = 0;
    std::string file;          // DIFile filename
    bool artificial = false;
};

struct Module {
    std::vector<Function> functions;           // definitions only
    std::vector<std::string> declarations;      // declared symbol names
    std::map<std::string, DILoc> locs;          // "!13" -> line/col
    std::map<std::string, DISub> subprograms;   // "!10" -> source name/line/file
    const Function* find(std::string_view name) const;
};

PRISM_API Module parse_module(std::string_view text);
PRISM_API Type parse_type(std::string_view text);

}  // namespace ir

// ---------------------------------------------------------------------------
// PIR
// ---------------------------------------------------------------------------

enum class Op {
    Copy, Havoc,
    Add, Sub, Mul, UDiv, SDiv, URem, SRem, Shl, LShr, AShr, And, Or, Xor,
    Eq, Ne, Ult, Ule, Ugt, Uge, Slt, Sle, Sgt, Sge,
    Select, ZExt, SExt, Trunc,
    SMax, SMin, UMax, UMin, Abs, Ctlz, Cttz, Ctpop, Bswap,
    // i1 predicates used by property instrumentation (1 = the bad case)
    SAddOvf, SSubOvf, SMulOvf, UAddOvf, USubOvf, UMulOvf,
    SDivOvf,      // a == INT_MIN && b == -1
    ShiftOob,     // b >=u width
    ShlSOvf,      // C signed shl base: a < 0 || a * 2^b not representable
    ShlNswOvf,    // (a << b) >>s b != a
    ShlNuwOvf,    // (a << b) >>u b != a
    LostBitsL,    // exact lshr: (a >>u b) << b != a
    LostBitsA,    // exact ashr
    InexactU,     // exact udiv: a %u b != 0
    InexactS,     // exact sdiv: a %s b != 0
};

PRISM_API const char* op_name(Op op);

struct Arg {
    bool is_const = false;
    unsigned width = 0;
    uint64_t bits = 0;         // constant (masked to width)
    int var = -1;              // variable index when !is_const
    static Arg c(unsigned w, uint64_t v);
    static Arg v(int var, unsigned w);
};

struct Var {
    std::string name;
    unsigned width = 0;
};

struct Stmt {
    enum Kind { Assign, Check, Assume };
    Kind kind = Assign;
    int dst = -1;              // Assign
    Op op = Op::Copy;
    std::vector<Arg> args;     // Check/Assume: args[0] is the i1 condition
    bool uninit = false;       // Havoc of an uninitialised local
    bool nondet = false;       // Havoc of a nondet source (__VERIFIER_nondet_*)
    // Check: violation when args[0] == 1 (PIR `check` = assert(!args[0])).
    std::string prop;          // "ovf+", "div0", "shift", ...
    std::string cls;           // taxonomy class (INT-SIGNED-OVF ...)
    std::string msg;
    int line = 0;
};

struct Phi {
    int dst = -1;
    std::vector<std::pair<int, Arg>> in;  // (pred block, value)
};

struct Term {
    enum Kind { Jmp, Br, Ret, Stop };      // Stop: path ends without returning (exit/abort)
    Kind kind = Stop;
    Arg cond;
    int t = -1, f = -1;
    std::optional<Arg> val;
};

struct Block {
    std::string name;
    std::vector<Phi> phis;
    std::vector<Stmt> stmts;
    Term term;
};

struct Function {
    std::string name;          // source name
    std::string ir_name;       // IR symbol
    std::string file;
    int line = 0;
    std::vector<Var> vars;
    std::vector<int> params;   // var indices, in order
    unsigned ret_width = 0;    // 0: void
    std::vector<Block> blocks; // blocks[0] is the entry
    bool nondet = false;       // uses a nondet source (translation validation partial)
    std::vector<std::string> inlined;
};

struct Translation {
    std::optional<Function> fn;
    std::string status;        // "" ok, else laws::NEEDS_HARNESS
    std::string reason;        // "UNENCODED: <construct>" / pointer parameter message
    std::vector<int> folded_used;  // indices of TranslateOptions::folded checked here
};

struct FoldedUb {              // clang constant-folded UB (diagnostic at line:col)
    int line = 0;
    int col = 0;
    std::string cls;
    std::string msg;
};

struct TranslateOptions {
    std::vector<std::pair<int, int>> signed_shl;  // (line, col) of C signed `<<`
    std::vector<FoldedUb> folded;                 // @__prism.folded(i32 k) -> folded[k]
    int inline_depth = 4;
};

PRISM_API Translation translate(const ir::Module& m, const ir::Function& f,
                                const TranslateOptions& opt = {});
PRISM_API std::string to_text(const Function& fn);
PRISM_API std::string sha256_hex(std::string_view data);

// ---------------------------------------------------------------------------
// Interpreter (translation validation, roadmap 2.4)
// ---------------------------------------------------------------------------
struct InterpResult {
    enum Status { Returned, Stopped, Violation, AssumeFailed, StepLimit };
    Status status = Returned;
    uint64_t ret = 0;
    bool ret_nondet = false;   // returned value depends on a havoc / undef
    std::string prop;          // Violation: property name
    std::string cls;
    int line = 0;
};

// Concrete semantics of one PIR operator at result width w (argument widths
// aw). Division by zero follows Z3's total bvudiv/bvsdiv so the interpreter
// and the encoder agree even on paths a check already rejected.
PRISM_API uint64_t eval_op(Op op, unsigned w, const std::vector<uint64_t>& a,
                           const std::vector<unsigned>& aw);

PRISM_API InterpResult interpret(const Function& fn, const std::vector<uint64_t>& args,
                                 uint64_t step_limit = 2'000'000);

// ---------------------------------------------------------------------------
// Encoder (Z3 bitvectors)
// ---------------------------------------------------------------------------

// One verification condition: SAT of `smt2` means the property is violated
// (or, for kind "unwind", that a loop runs past the bound). Solver-neutral
// SMT-LIB2 so a certified back end (src/prism/solver/) can consume it.
struct Vc {
    std::string kind;          // "property" | "unwind"
    std::string prop;
    std::string cls;
    std::string msg;
    int line = 0;
    std::string smt2;
};

struct Verdict {
    std::string status;        // laws::FAILED / PROVED / BOUNDED / PROVED_UNBOUNDED / UNKNOWN / ERROR
    std::string message;
    std::string cls;
    std::string prop;
    int line = 0;
    std::map<std::string, uint64_t> cex;       // param name -> bits
    std::vector<uint64_t> cex_args;            // in parameter order
    std::map<std::string, std::string> extra;  // unwind, unwind_closed, k_induction ...
};

PRISM_API bool z3_available();
PRISM_API std::vector<Vc> pir_vcs(const Function& fn, int unwind = 8);
PRISM_API Verdict check_function(const Function& fn, int unwind, double timeout_s = 30.0);
// "name=value, ..." with signed decimal values (the bmc stage's format).
PRISM_API std::string format_cex(const Function& fn, const std::vector<uint64_t>& args);

// ---------------------------------------------------------------------------
// Stage
// ---------------------------------------------------------------------------

struct Frontend {
    std::optional<std::filesystem::path> clang, clangxx, opt, lli;
    std::string version;       // "clang-18"
};
PRISM_API Frontend find_frontend(const Config& cfg);

// Lower one C/C++ file to normalised LLVM IR text. Returns nullopt and fills
// err on failure. diags receives clang's warnings (folded-UB attribution).
PRISM_API std::optional<std::string> lower_to_ir(const Frontend& fe,
                                                 const std::filesystem::path& src,
                                                 double timeout_s, std::string& err,
                                                 std::vector<FoldedUb>* folded = nullptr,
                                                 std::vector<std::pair<int, int>>* signed_shl = nullptr);

// The pir stage: one finding per defined function of every C/C++ unit.
PRISM_API std::vector<Finding> run_pir(const std::vector<std::filesystem::path>& sources,
                                       const Config& cfg);

}  // namespace prism::pir
