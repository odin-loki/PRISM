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

struct Operand;

struct Value {
    // Aggregate: [..] / { .. } constant (elems); Str: c"..." (bytes);
    // ConstExpr: getelementptr / ptrtoint / inttoptr / bitcast (...) with
    // ce_op, ce_flags, ce_ty (GEP source element type / cast target), elems.
    // Fp: floating-point literal, IEEE bits of its type in `bits`.
    enum Kind { Local, Global, Int, Undef, Poison, Null, Zero, Other, Aggregate, Str, ConstExpr, Fp };
    Kind kind = Other;
    std::string name;          // Local/Global name without sigil
    uint64_t bits = 0;         // Int: two's complement bits (masked by user)
    bool negative = false;     // Int literal was written negative
    std::string text;          // as written (constant expressions etc.)
    std::string bytes;         // Str: decoded bytes
    std::string ce_op;
    std::vector<std::string> ce_flags;
    Type ce_ty;
    std::vector<Operand> elems;
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
    std::string loop_md;       // "!15": the !llvm.loop attachment of a back-edge branch
    std::string text;          // original text (messages)
    bool parsed = true;        // false: opcode known, operands not modelled
    // memory instructions (docs/PIR.md "Memory model"):
    //   alloca:        ety = allocated type, ops = [count] (optional)
    //   load:          ty = loaded type, ops = [ptr]
    //   store:         ops = [value, ptr]
    //   getelementptr: ety = source element type, ops = [base, idx...]
    Type ety;
    unsigned align = 0;        // alloca/load/store `align N` (0 = absent)
    // control flow (docs/PIR.md "Exceptions", "Indirect calls", "Inline assembly")
    std::optional<Operand> callee_op;  // call/invoke through a local pointer
    std::vector<std::pair<std::string, Operand>> clauses;  // landingpad: ("catch"|"filter", typeinfo)
    bool is_asm = false;       // call asm "..."
    std::string asm_text;
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
    bool is_model = false;     // linked from the library models (link_models)
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

struct Global {
    std::string name;
    Type ty;                   // value type
    bool is_const = false;     // `constant`
    bool external = false;     // declaration only (no initializer)
    bool thread_local_ = false;
    std::vector<Operand> init; // [initializer] (empty when external)
    unsigned align = 0;
    std::string text;          // declaration line (messages)
};

struct Module {
    std::vector<Global> globals;
    std::string datalayout;                    // target datalayout string
    std::map<std::string, Type> types;         // named struct types ("struct.S" -> body; opaque: kind Other)
    std::vector<Function> functions;           // definitions only
    std::vector<std::string> declarations;      // declared symbol names
    std::map<std::string, DILoc> locs;          // "!13" -> line/col
    std::map<std::string, DISub> subprograms;   // "!10" -> source name/line/file
    std::map<std::string, DILoc> loop_starts;   // "!15" (llvm.loop) -> the loop's start location
    const Function* find(std::string_view name) const;
    const Global* find_global(std::string_view name) const;
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
    // Memory queries (read the memory state at this point; docs/PIR.md
    // "Memory model"). Argument: a pointer (i64: object id << 48 | offset).
    ObjSize,      // i64: byte size of the pointer's object (0: null / no object)
    ObjLive,      // i1: the object is allocated and not freed / out of scope
    ObjKind,      // i8: MemKind of the object (0: null / no object)
    ObjAlign,     // i64: alignment of the object's base address
    // IEEE floating point on the bits of half/float/double (width 16/32/64),
    // round to nearest even (docs/PIR.md "Floating point"; src/prism/pir/fp.hpp).
    FAdd, FSub, FMul, FDiv, FRem, FSqrt, FFma, FMulAdd,
    FMinNum, FMaxNum, FMinimum, FMaximum,
    FFloor, FCeil, FTruncI, FRoundA, FRoundE,
    FOeq, FOlt, FOle, FUno,          // i1 comparisons (the others are built from these)
    FToSI, FToUI, SIToF, UIToF, FConv,
    FToSIOvf, FToUIOvf,              // i1: value out of range of the iN in args[1] (UB)
    FIsNaN, FIsZero, FIsInf,         // i1 classification
    FLibm,                           // unmodelled libm function (msg = name): unconstrained result
};

// Allocation kinds (ObjKind values). Extern: an object PRISM assumes to
// exist (a pointer parameter under a contract, a FILE, a getenv string).
enum class MemKind : uint8_t { None = 0, Stack = 1, Heap = 2, Static = 3, Const = 4, New = 5, NewArr = 6, Extern = 7, File = 8 };
PRISM_API const char* mem_kind_name(MemKind k);

// Pointer encoding: bits 63..48 object id (0 = null object), bits 47..0 byte
// offset. Object sizes stay below 2^47, and every pointer arithmetic step is
// checked to stay inside its object (or one past the end), so the packed
// value and the (object, offset) pair of the Lean model coincide on every
// execution without a reported violation (docs/PIR.md).
inline constexpr unsigned kObjShift = 48;
inline constexpr uint64_t kOffMask = (uint64_t{1} << kObjShift) - 1;
inline constexpr uint64_t kMaxObjSize = uint64_t{1} << 47;
inline constexpr uint64_t kMaxObjects = 0xFFFF;
// Object ids [kFnObjBase, kMaxObjects) are function addresses (never
// allocated, never dereferenceable); allocations stay below kFnObjBase.
inline constexpr uint64_t kFnObjBase = 0xF000;

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
    bool fp = false;           // IEEE bits of a half/float/double value
};

struct Stmt {
    // Memory statements (docs/PIR.md "Memory model"):
    //   Alloc        dst := fresh object; args [size i64]; mkind, init, align
    //   Free         args [ptr]            object ends its lifetime (checks are separate)
    //   Load         dst := bytes at args[0] (width of dst, little endian);
    //                dst2 := i1: some loaded byte is uninitialised, or iN
    //                (N = bytes): bit k = byte k uninitialised (raw copies);
    //                dst3 (i1) := effective-type mismatch (strict aliasing)
    //   Store        args [ptr, value, init] init: i1 for all bytes, or iN
    //                (N = bytes) with bit k = byte k initialised
    //   MemCpy       args [dst, src, len i64]  (memmove semantics: reads before writes)
    //   MemSet       args [dst, byte i8, len i64]
    //   StackRestore args [token i64]      stack objects with id > token end
    //   StackSave    dst := token (i64)
    enum Kind { Assign, Check, Assume, Alloc, Free, Load, Store, MemCpy, MemSet, StackSave, StackRestore };
    Kind kind = Assign;
    int dst = -1;              // Assign
    Op op = Op::Copy;
    std::vector<Arg> args;     // Check/Assume: args[0] is the i1 condition
    bool uninit = false;       // Havoc of an uninitialised local
    bool nondet = false;       // Havoc of a nondet source (__VERIFIER_nondet_*)
    // Havoc of a program's own __VERIFIER_nondet_* call (not one inside a
    // library model): the callee, its C signedness and the call's source
    // column (`line` holds the line). The counterexample of a refutation
    // lists these values in call order (Verdict::extra["nondet"]).
    std::string nondet_fn;
    bool nondet_unsigned = false;
    int col = 0;
    // Check: violation when args[0] == 1 (PIR `check` = assert(!args[0])).
    std::string prop;          // "ovf+", "div0", "shift", ...
    std::string cls;           // taxonomy class (INT-SIGNED-OVF ...)
    std::string msg;
    int line = 0;
    MemKind mkind = MemKind::None;  // Alloc
    int init = 0;              // Alloc: 0 uninitialised, 1 zero-filled, 2 initialised with arbitrary bytes
    unsigned align = 1;        // Alloc: base alignment
    unsigned tag = 0;          // Load/Store: effective-type tag (strict aliasing), 0 = untyped
    int dst2 = -1, dst3 = -1;  // Load shadows
    bool ptr_arith = false;    // Assign add/copy/select: result stays in args[0]'s object (checked)
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
    bool uses_memory = false;  // has memory statements (k-induction is not attempted)
    bool mutable_globals = false;  // reads/writes a mutable global (translation validation partial)
    bool returns_ptr = false;
    std::vector<std::string> ptr_params;   // pointer parameters bound to contract objects
    std::vector<std::string> assumptions;  // PROVED becomes PROVED-ASSUMING when non-empty
    std::vector<std::string> throws;       // library throw calls whose paths end (not modelled)
    std::vector<std::string> libm;         // libm functions whose results are unconstrained
    // Source start (line, column) of the function's own loops (clang's
    // llvm.loop metadata on the back-edge branch), by the PIR blocks that
    // branch targets (one of them is the loop header). Inlined callees'
    // loops are not listed.
    std::map<int, std::pair<int, int>> loop_locs;
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

// A pointer-parameter precondition (Law 6): `// requires: \valid(p+(0..n-1))`
// (or ACSL `/*@ requires ... */`) or a drafted harness gives the size of the
// object p points to; p is then bound to a fresh object and a proof is
// PROVED-ASSUMING with the assumptions listed.
struct PtrContract {
    std::string param;         // IR parameter name
    int64_t count = -1;        // constant element count, or
    std::string count_param;   // element count taken from this integer parameter
    int64_t count_add = 0;     // count = count_param + count_add
    unsigned elem_bytes = 0;   // 0: from the IR (first load/store/GEP through p)
    bool read_only = false;    // \valid_read
    int64_t count_min = INT64_MIN, count_max = INT64_MAX;  // range assumed for count_param (drafted harness)
    std::string text;          // the clause as written
    std::string source;        // "requires" | "harness"
};

struct TranslateOptions {
    std::vector<std::pair<int, int>> signed_shl;  // (line, col) of C signed `<<`
    std::vector<FoldedUb> folded;                 // @__prism.folded(i32 k) -> folded[k]
    int inline_depth = 4;
    // pointer parameters of the function being translated
    std::vector<PtrContract> contracts;
    // Mutable globals start at their initializer (program entry, `main`).
    // Otherwise their contents are arbitrary initialised bytes.
    bool globals_initial = false;
    // --strict-aliasing: effective-type tags per byte (C11 6.5p6-7), opt-in.
    bool strict_aliasing = false;
    // --fp-checks: IEEE exceptions (division by zero, invalid, overflow) as warnings, opt-in.
    bool fp_checks = false;
    // `// prism: asm ensures <cond>` contracts: source line of the asm -> cond
    std::map<int, std::string> asm_contracts;
};

PRISM_API Translation translate(const ir::Module& m, const ir::Function& f,
                                const TranslateOptions& opt = {});
PRISM_API std::string to_text(const Function& fn);
// Appends (LLVM fragment, PIR) for the Lean correspondence checker to
// <dir>/<unit>.pirl when PRISM_PIR_LEAN_EXPORT is set (src/prism/pir/export_lean.cpp).
PRISM_API void export_lean_pair(const std::filesystem::path& out_root, const std::string& unit,
                                const ir::Module& m, const ir::Function& f, const TranslateOptions& opt,
                                const Translation& t);
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

// Memory encoding (docs/PIR.md "Memory model"):
//   Array  one SMT array from address (object id, offset) to byte cell
//          (unbounded; QF_ABV + lambdas for ranged copies)
//   Bv     Ackermannised read-over-write chains: every load is an ite chain
//          over the guarded writes before it, and arbitrary initial bytes are
//          fresh variables with pairwise consistency constraints (QF_BV, the
//          form the certified back end accepts; mirrors PrismSem/MemEncode)
enum class MemEncoding { Array, Bv };

struct EncodeOptions {
    MemEncoding memory = MemEncoding::Bv;  // QF_BV; Array is the unbounded alternative (slower on ranged copies)
};

PRISM_API bool z3_available();
// pir_vcs always uses MemEncoding::Bv unless told otherwise (QF_BV VCs).
PRISM_API std::vector<Vc> pir_vcs(const Function& fn, int unwind = 8);
PRISM_API std::vector<Vc> pir_vcs(const Function& fn, int unwind, const EncodeOptions& eo);
// How check_function answers its verification conditions (docs/PIR.md
// "Solving"). Every property VC and the unwinding assertion is one query to
// prism::solver::solve (include/prism/solver.hpp): the portfolio, the query
// cache and, with `certified`, CaDiCaL LRAT proofs checked by cake_lpr (and
// also by Lean's verified LRAT checker when the Lean-proved bit-blaster made
// the CNF: SolveOptions::bitblaster = auto). A function with no VC at all
// stays PROVED with certify_note: a certificate that checks nothing is not one.
//
// Certified mode first tries ONE certificate for the whole function
// (`certify_combined`, roadmap 3.2 speed-up): the query "some VC is
// violated" (the disjunction of every property VC and the unwinding
// assertion under the shared path encoding) is UNSAT exactly when every VC
// is UNSAT, so one LRAT proof of it, checked like any other, certifies the
// same claim as one proof per VC. The certificate record lists the VCs it
// covers (certificate_scope = combined, certificate_covers). If that query
// is SAT, has no answer, or is not certified, the per-VC path runs as
// before (nothing from the combined attempt is used but its note).
struct CheckOptions {
    int unwind = 8;
    double timeout_s = 30.0;    // per verification condition
    bool portfolio = true;      // false: Z3 alone (plus CaDiCaL when certified)
    bool certified = false;     // roadmap 3.2: PROVED-CERTIFIED when every VC is certified
    bool certify_combined = true;  // certified: one combined certificate first (2+ VCs)
    double check_timeout_s = 0;    // certified: LRAT checker budget (0: the solver library default)
    // certified: seconds all certificate queries of one PROVED function may
    // take together (CaDiCaL-with-LRAT and the checkers; the plain answers
    // are not counted). Spent: the verdict stays PROVED with a certify_note.
    // 0: no budget beyond each query's own.
    double certify_budget_s = 0;
    bool use_cache = true;
    std::string cache_dir;      // empty: the solver library's default
    unsigned max_parallel = 0;  // solver members at once; 0: hardware threads
    std::vector<std::string> tool_dirs;  // searched first (tests); see SolveOptions
    bool search_default_tools = true;
    EncodeOptions encode;       // memory encoding (Bv: QF_BV, certifiable)
};
PRISM_API Verdict check_function(const Function& fn, const CheckOptions& opt);
// The portfolio without the query cache (library callers and tests).
PRISM_API Verdict check_function(const Function& fn, int unwind, double timeout_s = 30.0);
PRISM_API Verdict check_function(const Function& fn, int unwind, double timeout_s, const EncodeOptions& eo);
// "name=value, ..." with signed decimal values (the bmc stage's format).
PRISM_API std::string format_cex(const Function& fn, const std::vector<uint64_t>& args);

// ---------------------------------------------------------------------------
// Stage
// ---------------------------------------------------------------------------

struct Frontend {
    std::optional<std::filesystem::path> clang, clangxx, opt, lli;
    std::string version;       // "clang-18"
    // C++ library model headers (src/prism/pir/models/cxx, written out by the
    // pir stage): searched before libstdc++ when non-empty (docs/PIR.md
    // "C++ library models"). Empty: the platform library only.
    std::filesystem::path cxx_models;
};
PRISM_API Frontend find_frontend(const Config& cfg);

// Lower one C/C++ file to normalised LLVM IR text. Returns nullopt and fills
// err on failure. diags receives clang's warnings (folded-UB attribution).
PRISM_API std::optional<std::string> lower_to_ir(const Frontend& fe,
                                                 const std::filesystem::path& src,
                                                 double timeout_s, std::string& err,
                                                 std::vector<FoldedUb>* folded = nullptr,
                                                 std::vector<std::pair<int, int>>* signed_shl = nullptr,
                                                 std::string* cxx_models = nullptr);
// cxx_models receives which C++ library the unit was lowered with when
// fe.cxx_models is set: "" (the unit uses no modelled header), "model: vector"
// or "libstdc++ (fallback: <why>)" when the unit does not compile against the
// models (it is then lowered with the platform library, never skipped).

// ---------------------------------------------------------------------------
// Library models (roadmap 2.6, docs/PIR.md "Library models")
// ---------------------------------------------------------------------------

// Sources of the operational models, embedded in the binary at build time:
// (file name, text). C models (src/prism/pir/models/libc/*.c, prism_model.h)
// by file name; C++ library model headers as "cxx/<header>".
PRISM_API const std::vector<std::pair<std::string, std::string>>& model_sources();
// Write the C++ model headers ("cxx/..." sources) into dir; false on an I/O error.
PRISM_API bool write_cxx_models(const std::filesystem::path& dir);

struct ModelLibrary {
    std::vector<ir::Module> units;  // lowered model files
    std::string error;              // non-empty: the models could not be built
};
// Lower the embedded models with the same clang/opt pipeline (once per run).
PRISM_API ModelLibrary build_models(const Frontend& fe, double timeout_s);
// Link into m every model function m declares but does not define
// (transitively, with the globals they use). Returns the linked names.
PRISM_API std::vector<std::string> link_models(ir::Module& m, const ModelLibrary& lib);

// ---------------------------------------------------------------------------
// Pointer-parameter contracts (Law 6, docs/PIR.md "Pointer parameters")
// ---------------------------------------------------------------------------

// `// requires: \valid(p + (0..n-1))`, `\valid(p)`, `\valid_read(...)`, and
// ACSL `/*@ requires \valid(...); */` clauses in the comment block right
// before the function (or its first body lines). Unrecognised requires
// clauses are returned in *unparsed (the function stays NEEDS-HARNESS for
// the pointers they would cover).
PRISM_API std::vector<PtrContract> parse_contracts(const std::vector<std::string>& source_lines, int fn_line,
                                                   const ir::Function& f,
                                                   std::vector<std::string>* unparsed = nullptr);

// The pir stage: one finding per defined function of every C/C++ unit.
PRISM_API std::vector<Finding> run_pir(const std::vector<std::filesystem::path>& sources,
                                       const Config& cfg);

// Tooling (tools/solver_bench.py; src/prism/pir/bench.cpp). JSON on return.
// Every VC of every encodable function of `src`, written as SMT-LIB2 files
// under out_dir; functions that are not encoded are listed with their status.
PRISM_API std::string unit_vcs_json(const std::filesystem::path& src, const Config& cfg,
                                    const std::filesystem::path& out_dir);
// One VC through prism::solver::solve (Z3 alone when z3_only; cfg.timeout,
// cfg.certified). The query cache is off (a timing tool); cfg.solver_cache
// is where the scheduler's solve-time history is kept.
PRISM_API std::string solve_smt2_json(const std::string& smt2, const Config& cfg, bool z3_only);

}  // namespace prism::pir
