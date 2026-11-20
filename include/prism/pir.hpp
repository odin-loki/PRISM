#pragma once

// PIR: the PRISM intermediate representation (roadmap Part 2, docs/PIR.md).
//
// Pipeline: Clang -> LLVM IR (-O0, optnone off) -> fixed normalisation passes
// (mem2reg, lower-switch, loop-simplify, lcssa, instnamer) -> textual IR
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <functional>
#include <map>
#include <set>
#include <memory>
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
    uint64_t hi = 0;           // Int of a type wider than 64 bits: bits 127..64 (two's complement, sign-extended)
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
    std::string asm_constraints;  // the asm's constraint string ("=r,r,~{memory}")
    bool asm_sideeffect = false;  // `asm sideeffect` (volatile)
    // Lowered by the parser into plain instructions (atomics, fences, asm
    // goto): kernel_note names what was assumed; without --kernel-api the
    // translator refuses the instruction as before.
    std::string kernel_note;
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
    bool variadic = false;     // defined with `...` (models only: extra call arguments are evaluated and dropped)
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
    std::map<std::string, std::string> md_strings;  // "!12" -> "rsp": one-string tuples !{!"rsp"} (named registers)
    const Function* find(std::string_view name) const;
    const Global* find_global(std::string_view name) const;
};

PRISM_API Module parse_module(std::string_view text);
// kernel mode (--kernel-api): also models the atomic and barrier forms; parse_module leaves
// them unparsed, so the conc stage reads their raw text as before
PRISM_API Module parse_module_kernel(std::string_view text);
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

// Pointer encoding (LP64; ILP32 in PtrModel below): bits 63..48 object id
// (0 = null object), bits 47..0 byte offset. Object sizes stay below 2^47, and every pointer arithmetic step is
// checked to stay inside its object (or one past the end), so the packed
// value and the (object, offset) pair of the Lean model coincide on every
// execution without a reported violation (docs/PIR.md).
// Pointer model of a function (LP64 = the constants below). ILP32
// (--data-model ILP32, i386 datalayout `p:32:32`) packs a 32-bit pointer:
// object id in bits 31..22, byte offset in bits 21..0. An object of 2^21
// bytes or more, or more objects than the id space holds, is UNKNOWN (never
// a proof): every such limit is checked where an object is allocated.
struct PtrModel {
    unsigned w;              // pointer width in bits (= size_t, ptrdiff_t)
    unsigned obj_shift;      // object id in bits w-1..obj_shift
    uint64_t max_obj_size;   // exclusive bound on an object's size
    uint64_t max_objects;    // ids [1, fn_obj_base) allocations, [fn_obj_base, max_objects) functions
    uint64_t fn_obj_base;
    uint64_t off_mask() const { return (uint64_t{1} << obj_shift) - 1; }
    uint64_t make(uint64_t obj, uint64_t off) const { return (obj << obj_shift) | (off & off_mask()); }
    uint64_t obj_of(uint64_t p) const { return p >> obj_shift; }
    uint64_t off_of(uint64_t p) const { return p & off_mask(); }
};
inline constexpr PtrModel kLP64{64, 48, uint64_t{1} << 47, 0xFFFF, 0xF000};
inline constexpr PtrModel kILP32{32, 22, uint64_t{1} << 21, 0x3FF, 0x3C0};
// The pointer model of the function being translated or encoded (thread
// local; LP64 outside a PtrModelScope). translate() and the encoder open a
// scope with fn.ptr, so the helpers below follow the function.
PRISM_API const PtrModel& ptr_model();
class PRISM_API PtrModelScope {
public:
    explicit PtrModelScope(const PtrModel& m);
    ~PtrModelScope();
    PtrModelScope(const PtrModelScope&) = delete;
    PtrModelScope& operator=(const PtrModelScope&) = delete;

private:
    PtrModel prev_;
};
inline unsigned ptr_w() { return ptr_model().w; }
inline unsigned obj_shift() { return ptr_model().obj_shift; }
inline uint64_t off_mask() { return ptr_model().off_mask(); }
inline uint64_t max_obj_size() { return ptr_model().max_obj_size; }
inline uint64_t max_objects() { return ptr_model().max_objects; }
inline uint64_t fn_obj_base() { return ptr_model().fn_obj_base; }


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
    //   Revive       args [ptr]            a stack object's lifetime starts again
    //                (llvm.lifetime.start; other objects: no effect; its bytes
    //                are set separately, checks are separate)
    //   MemHavoc     no args: every object's bytes get arbitrary values and
    //                tags, and an initialised byte stays initialised (initialised
    //                before or arbitrary); sizes, liveness, kinds and alignment
    //                are unchanged (inline asm with a "memory" clobber)
    enum Kind {
        Assign, Check, Assume, Alloc, Free, Load, Store, MemCpy, MemSet, StackSave, StackRestore, Revive, MemHavoc
    };
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
    PtrModel ptr = kLP64;      // from the module's datalayout (translate)
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

// A folded diagnostic that is not undefined behaviour in a -fwrapv / -fno-strict-overflow build (--pir-build-semantics):
// clang's -Winteger-overflow (signed + - * of constants), -Wshift-sign-overflow, -Wshift-negative-value and
// -Wshift-overflow (signed << results). Shift-amount and division diagnostics stay UB.
PRISM_API bool wraps_under_fwrapv(const FoldedUb& f);

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

// Automatic pointer harness (--pir-harness, src/prism/pir/harness.cpp): a
// pointer parameter without a `requires` clause (or draft) is bound to an
// object built from the IR types, every choice a named assumption.
struct AutoHarness {
    bool on = false;
    int depth = 2;                 // nested pointer levels below the parameter's own object
    uint64_t elems = 16;           // default element count of an array-like pointee
    uint64_t unknown_bytes = 64;   // default size when the pointee type is not determined
    int max_objects = 32;          // objects per pointer parameter
};

// --ops-table FILE (docs/PIR.md "Indirect calls through ops tables"): one field of a
// known ops table. struct_type is the IR spelling of the struct ("%struct.NAME"); index
// is the field's position: the 5th column of the file, else its 0-based position among
// the distinct fields the file lists for that struct.
struct OpsField {
    std::string struct_type;
    std::string field;
    int index = 0;
    std::vector<std::string> targets;
    std::string role;
};
// --pir-field-invariants FILE: one value invariant of a struct field, applied to every automatic-harness object of
// that struct type (a named assumption per row used). TSV: struct ("%struct.NAME"), byte offset, width (bytes),
// kind, a, b, status, source. Kinds: "valid" (a pointer field points to a fresh object of `a` bytes, arbitrary
// contents), "nonnull" (a pointer field is not NULL), "range" (an integer field, signed, a <= v <= b).
// An optional 9th column scopes the row: "fn:GLOB" (the analysed function's name), "unit:GLOB" (the compile unit's
// path) or "module:GLOB" (the unit's path as well, for a directory such as "*/drivers/net/ethernet/intel/*");
// empty = every function (global). A row applies only where its scope matches (field_invariant_applies), and the
// assumption names the scope. '*' and '?' are the glob characters; '*' also matches '/'.
struct FieldInvariant {
    std::string struct_type;
    uint64_t offset = 0;
    unsigned width = 0;
    std::string kind;
    int64_t a = 0, b = 0;
    std::string status, source;
    std::string scope;
};
PRISM_API bool field_invariant_applies(const FieldInvariant& fi, const std::string& function, const std::string& unit_path);
// --pir-callback-models FILE (docs/DRIVER_ENV_MODELS.md): driver environment models. One row is one precondition the
// kernel core guarantees for one parameter of one kind of driver callback (adapted from the Klever / LDV environment model
// specifications, Apache-2.0, attribution in the file). TSV, 10 columns:
//   interface  container  field_off  callback  param  kind  a  b  status  source
// container: "%struct.T" (the callback is a function address stored at byte `field_off` of a T, in a constant initialiser
// or by a store in the unit) or "call:FN" (the callback is argument `field_off` (0-based) of a direct call of @FN in the
// unit: request_irq, timer_setup, INIT_WORK ...). Kinds (each use is a named assumption, nothing is checked here):
//   range a b         the integer parameter is in [a, b] (signed)
//   id-entry a        the parameter points to an element 0 .. n-2 of the array the container instance's field at byte
//                     `a` points to (the driver's own constant id table, last entry the terminator); needs exactly one
//                     instance of the container in the unit naming the callback, and that table in the unit
//   trailing-alloc a b  the parameter's object (of struct type b, "%struct.net_device") has the private area its allocator
//                     appends: a = "fn:arg,fn:arg" (alloc_netdev_mqs:0 ...); the size is the constant argument of the unit's
//                     calls of those functions, applied only when all of them agree
//   buffer a          the pointer parameter points to the start of a fresh buffer of `a` bytes, arbitrary contents
//                     (sysfs show / store get a PAGE_SIZE buffer)
//   drvdata a b       probe-first ordering: container/field_off name the driver's probe; a function the unit registers at
//                     another field of that container, in a dev_pm_ops or as a device attribute reads driver_data (byte `a`
//                     of the device struct `b`, "%struct.device") that the probe set. When the probe's own body stores into
//                     it (directly or through setters: platform_set_drvdata -> dev_set_drvdata ...) only results of one
//                     constant-size allocation (devm_kzalloc, kzalloc ...), the harness object of that field is exactly
//                     that allocation. param "*": every pointer parameter whose harness object holds a `b`
// field_off "*": every field of the container (with trailing-alloc the struct type check selects the parameters).
// source: where the precondition comes from ("Klever spec drivers/pci/pci event spec.json", or a kernel document).
struct CallbackModel {
    std::string interface_, container, callback, kind, a, b, status, source;
    uint64_t field_off = 0;
    int param = 0;
};
// The models of one function (resolved against its unit): what translate() applies to parameter `param`.
struct CallbackBinding {
    int param = 0;
    std::string kind;          // range | id-entry | trailing-alloc | buffer | drvdata (param -1: any pointer parameter)
    int64_t lo = 0, hi = 0;    // range
    std::string global;        // id-entry: the id table global
    uint64_t elem_bytes = 0, count = 0;  // id-entry: element size, number of selectable entries (n - 1)
    uint64_t extra_bytes = 0;  // trailing-alloc: private area; buffer: buffer size
    std::string struct_type;   // trailing-alloc: the object's struct type; drvdata: the device struct
    uint64_t field_off = 0;    // drvdata: byte offset of driver_data in the device struct
    std::string alloc_type;    // drvdata: the struct the probe accesses its allocation as ("" when none)
    std::string text;          // the named assumption
};
// Reads a --pir-callback-models file. On failure returns {} and sets error.
PRISM_API std::vector<CallbackModel> load_callback_models(const std::filesystem::path& file, std::string& error);
// The models that apply to `f`: the (container, offset) sites the unit registers `f` at, matched against the rows.
// `skipped` (optional) gets one line per matching row that could not be applied and why.
PRISM_API std::vector<CallbackBinding> resolve_callback_bindings(const ir::Module& m, const ir::Function& f,
                                                                 const std::vector<CallbackModel>& models,
                                                                 std::vector<std::string>* skipped = nullptr);
// One lock contract of --lock-contracts (TSV, docs/KERNEL_CONCURRENCY.md section 14), for
// unmodified code: the analysed (root) function acquires, releases or requires a lock.
struct LockContract {
    std::string function;  // IR name of the analysed function
    std::string lock;      // "global:NAME" (a module global's address) or "arg:N" (0-based parameter)
    std::string action;    // "acquires" (returns with it held) | "releases" (held on entry) | "requires-held" (both)
    bool spin = false;     // kind "spin": a spinlock (atomic depth), otherwise a mutex
};
// Parses the TSV: function TAB lock TAB action [TAB kind]; '#' lines, blanks and a "function" header are skipped.
// On a bad line *error gets "line N: reason" and the result is empty.
PRISM_API std::vector<LockContract> parse_lock_contracts(std::string_view tsv, std::string* error = nullptr);
// One object of an automatic harness as a call-site obligation (--pir-discharge):
// the caller must pass a pointer to byte `member_off` of a live object of at least
// `bytes` bytes aligned to 16 whose bytes are initialised, and the listed pointer
// fields must hold pointers that satisfy the child obligation.
struct ObligationNode {
    std::string name;
    uint64_t bytes = 0;
    uint64_t member_off = 0;  // container_of: the pointer points this far into the object
    unsigned align = 16;      // the object's alignment the callee's accesses need (the harness gives 16)
    bool check_init = true;   // the bytes are checked initialised (objects over the cap keep that as an assumption)
    std::vector<std::pair<uint64_t, std::shared_ptr<ObligationNode>>> nested;
};
// callee (IR name) -> per parameter index the obligation (nullptr: none)
using CallObligations = std::map<std::string, std::vector<std::shared_ptr<ObligationNode>>>;

// One contract of --callee-contracts for a callee without a body (docs/HARNESS_PHASE2.md). TSV, 7 columns:
//   function  signature  requires  assigns  ensures  status  source
// signature: `name:type,...->type` with type ptr, bool, u8/u16/u32/u64, s8/s16/s32/s64 (parameter names; ACSL style
// clauses refer to them). Clauses are separated by `;`. requires: boolean expressions over the parameters, and
// `\valid_bytes(p, n)` / `\valid_read_bytes(p, n)`. assigns: `\nothing`, `\anything`, `\bytes(p, n)`; empty = the pointer
// arguments' objects are havocked (as --assume-unknown-calls, named). ensures: boolean expressions over `\result` and the
// parameters, `\false` (the call does not return), and for a pointer result `\valid_bytes(\result, n)` (a fresh object),
// optionally `\result == \null || \valid_bytes(\result, n)`. status: UNVALIDATED, REVIEWED or VALIDATED, always shown.
struct CalleeContract {
    std::string function;
    std::string signature;
    std::string requires_;
    std::string assigns;
    std::string ensures;
    std::string status;
    std::string source;
};
PRISM_API std::map<std::string, CalleeContract> load_callee_contracts(const std::filesystem::path& file, std::string& error);

struct TranslateOptions {
    // --pir-discharge: a translation that keeps only the obligations of these
    // callees at their direct call sites (every other check is dropped).
    CallObligations obligations;
    bool container_of = false;     // --pir-container-of
    // C++ before C++20 (CWG 1457): a located signed `<<` is UB when the base
    // is negative or the result does not fit the unsigned type; C: past the sign bit.
    bool shl_cxx = false;
    std::vector<std::pair<int, int>> signed_shl;  // (line, col) of C signed `<<`
    std::vector<FoldedUb> folded;                 // @__prism.folded(i32 k) -> folded[k]
    int inline_depth = 4;
    // Frames of PRISM's own library models (kernel specs, libc models: ir::Function::is_model) do not count toward
    // inline_depth: a model is inlined exactly like any callee, so this only adds encoding, never an assumption. They
    // have their own bound: the whole inline stack may hold at most inline_depth + model_inline_depth frames, and a call
    // past either bound is still UNENCODED (NEEDS-HARNESS). 0 restores the old rule (models count like user code).
    // PRISM_PIR_MODEL_INLINE_DEPTH sets it (stage_mem.cpp).
    int model_inline_depth = 16;
    // A callee whose whole call tree has at most this many IR instructions is inlined past inline_depth (exactly, no
    // assumption), within the same total cap; 0 = off. PRISM_PIR_SMALL_INLINE sets it (stage_mem.cpp).
    int small_inline_insts = 64;
    // pointer parameters of the function being translated
    std::vector<PtrContract> contracts;
    // --lock-contracts: lock contracts of the root function (translate_lock.inc lock_table)
    std::vector<LockContract> lock_contracts;
    AutoHarness auto_harness;
    // Mutable globals start at their initializer (program entry, `main`).
    // Otherwise their contents are arbitrary initialised bytes.
    bool globals_initial = false;
    // --strict-aliasing: effective-type tags per byte (C11 6.5p6-7), opt-in.
    bool strict_aliasing = false;
    // --fp-checks: IEEE exceptions (division by zero, invalid, overflow) as warnings, opt-in.
    bool fp_checks = false;
    // `// prism: asm ensures <cond>` contracts: source line of the asm -> cond
    std::map<int, std::string> asm_contracts;
    // --kernel-api linux|freebsd (docs/KERNEL_MODELS.md): kernel API specs are
    // linked, and inline asm, asm goto, atomics and barriers are modelled
    // conservatively, each use a named assumption. "" = off (the default).
    std::string kernel_api;
    // --assume-unknown-calls: a call without a spec havocs its result and the
    // objects its pointer arguments point to, recorded as an assumption.
    // Opt-in, never on by default (docs/KERNEL_MODELS.md "Policy").
    bool assume_unknown_calls = false;
    // --ops-table FILE: the fields of known ops tables (empty = off). An indirect call
    // through one of them is checked against each of its targets; a field with more
    // than ops_max_targets targets stays UNENCODED ("too many indirect targets").
    std::vector<OpsField> ops_table;
    std::size_t ops_max_targets = 8;
    // --pir-icall-havoc (kernel mode): an indirect call with more than 16 candidate targets, or to a target outside the
    // unit's address-taken functions, is a havoc of all memory with an arbitrary result (named), not UNENCODED
    bool icall_havoc = false;
    // --pir-variadic: variadic functions are translated, va_start gives arbitrary variadic arguments (translate_va.inc, named)
    bool variadic = false;
    // --pir-recursion: a recursive call of a function on the inlining stack is that function's own verdict for the call's
    // arguments (induction on the recursion depth); its effects are a havoc of all memory and an arbitrary result (named)
    bool recursion = false;
    // --pir-kernel-sprintf (Linux kernel mode): sprintf with a literal format writes at most the format's bound, %p extensions included
    bool kernel_sprintf = false;
    // --pir-addrspace (kernel mode): loads and stores through ptr addrspace(N) are ordinary pointer accesses (named)
    bool addrspace = false;
    // --pir-wide-int: integers of 65..512 bits are bit-vectors of their width (otherwise UNENCODED)
    bool wide_int = false;
    // --pir-field-invariants: field value invariants applied to automatic-harness objects
    std::vector<FieldInvariant> field_invariants;
    // --pir-callback-models: the loaded rows; translate() resolves the ones that apply to the analysed function
    std::shared_ptr<const std::vector<CallbackModel>> callback_models;
    // --callee-contracts FILE (docs/HARNESS_PHASE2.md): assume-guarantee stubs for callees that have no body
    // (IR name -> contract). A callee with a body, or a kernel spec, is never replaced by one.
    std::map<std::string, CalleeContract> callee_contracts;
    // --pir-ops-auto: ops_table also holds the entries derived from the unit's own stores and initialisers
    // (OpsField::role "unit-derived"); a function-pointer field of an automatic harness is bound to one of its targets.
    bool ops_auto = false;
    // --pir-addr-values: ptrtoint / inttoptr constant expressions and integer-address initialisers are modelled
    bool addr_values = false;
    // --pir-builtin-contracts (kernel mode): contracts for @llvm.read_register / @llvm.prefetch / @llvm.objectsize
    bool builtin_contracts = false;
    // --pir-build-semantics with a unit built with -fwrapv / -fno-strict-overflow (lowered with -fwrapv, so clang emits no
    // nsw): signed `<<` into / past the sign bit or of a negative value wraps, and clang's folded signed-overflow and
    // signed-shift diagnostics are not UB. Shift amounts, division and memory are checked as before.
    bool wrapv = false;
    // --pir-kernel-stops (kernel mode): Linux BUG() and panic / vpanic / kassert_panic end the path (named assumption)
    bool kernel_stops = false;
    // --pir-kernel-current (kernel mode): the running thread's object is valid: Linux `current` (a %gs read of
    // current_task / const_current_task / pcpu_hot) and FreeBSD `curthread` (the %gs:0 pointer read of __curthread)
    bool kernel_current = false;
    // --pir-global-harness (kernel mode): mutable pointer globals point to a valid object at entry (named, per global)
    bool global_harness = false;
    // --pir-lock-wrappers (kernel mode): a root that only acquires (only releases) locks on every path is a lock
    // wrapper: LOCK-HELD-ON-RETURN (LOCK-UNHELD-UNLOCK) is not checked for it, as a named assumption
    bool lock_wrappers = false;
    // --pir-lock-scopes (kernel mode): a lock the root releases and never acquires anywhere in its call tree is held
    // by its caller on entry (named, per lock); one it acquires and never releases anywhere is returned held (named)
    bool lock_scopes = false;
    // --pir-bpf-helpers (Linux kernel mode): a BPF helper (the .func of a bpf_func_proto in the unit) gets, for each
    // argument the proto types ARG_PTR_TO_* without PTR_MAYBE_NULL, a valid object (named: the BPF verifier)
    bool bpf_helpers = false;
    // --pir-global-harness-deep (with --pir-global-harness): pointer fields of a harnessed global's object that the
    // function loads and uses get their own objects (auto-harness planner), and a list_head global is a valid
    // circular list (named, per global)
    bool global_harness_deep = false;
};

// Ops-table entries derived from the module itself (--pir-ops-auto): every `store ptr @f, getelementptr %struct.T, p, 0, K`
// and every function address in a constant initialiser of struct type T at field K makes @f a target of (T, K). The set is
// what THIS unit assigns: a field another unit assigns is not in it (a call through it then ends in the "target outside"
// stop, never in a silent proof). `role` is "unit-derived"; fields with more than max_targets targets are left out.
PRISM_API std::vector<OpsField> derive_ops_table(const ir::Module& m, std::size_t max_targets = 8);
// Adds the entries of `derived` for fields the explicit table does not list.
PRISM_API void merge_ops_tables(std::vector<OpsField>& explicit_table, const std::vector<OpsField>& derived);

// Reads a --pir-field-invariants file (format at FieldInvariant). On failure returns {} and sets error.
PRISM_API std::vector<FieldInvariant> load_field_invariants(const std::filesystem::path& file, std::string& error);
// PRISM_PIR_MINE_FIELDS: one JSON line per store to a struct field the module makes (field_mine.cpp): struct, offset,
// width, kind (alloc / global / const / null / other), value or size. Input of OS-Proving tools/prism_clusters/mine_fields.py.
PRISM_API void mine_field_stores(const ir::Module& m, const std::string& unit, std::ostream& out);

// Reads an --ops-table file (format in docs/PIR.md). On failure returns {} and sets error.
PRISM_API std::vector<OpsField> load_ops_table(const std::filesystem::path& file, std::string& error);

// LP64 unless the module's datalayout has 32-bit pointers (ILP32).
PRISM_API PtrModel ptr_model_of(const ir::Module& m);
PRISM_API Translation translate(const ir::Module& m, const ir::Function& f,
                                const TranslateOptions& opt = {});
PRISM_API std::string to_text(const Function& fn);
// Appends (LLVM fragment, PIR) for the Lean correspondence checker to
// <dir>/<unit>.pirl when PRISM_PIR_LEAN_EXPORT is set (src/prism/pir/export_lean.cpp).
PRISM_API void export_lean_pair(const std::filesystem::path& out_root, const std::string& unit,
                                const ir::Module& m, const ir::Function& f, const TranslateOptions& opt,
                                const Translation& t);
// Whether the function's translation is inside the Lean-proved LLVM fragment
// (the same test export_lean_pair applies: no `L unsupported <why>`), without
// writing any file. `reason` is the unsupported text, or why there is no
// translation at all. No change to the proofs: this only reports the export's verdict.
struct LeanCoverage {
    bool supported = false;
    std::string reason;
};
PRISM_API LeanCoverage lean_coverage(const ir::Module& m, const ir::Function& f, const TranslateOptions& opt,
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
// The same run, with `on_block(block, prev, values)` called at the entry of
// every block after its phis are assigned (prev: the predecessor, -1 at the
// entry). Returning false stops the run (status Stopped). Used by Houdini's
// concrete pre-filter (houdini.inc).
using InterpHook = std::function<bool(int block, int prev, const std::vector<uint64_t>& values)>;
PRISM_API InterpResult interpret(const Function& fn, const std::vector<uint64_t>& args, uint64_t step_limit,
                                 const InterpHook& on_block);

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
    // Bv: the arbitrary contents of a havoc entry (arbitrary initial bytes,
    // a havocked object, an asm memory clobber) as one uninterpreted function
    // of the address, instead of a fresh constant per read plus one pairwise
    // constraint per earlier read (quadratic; docs/PIR.md "Havoc reads as
    // functions"). Exact. The VCs are QF_UFBV, so check_function turns it off
    // in certified mode and pir_vcs never uses it. $PRISM_PIR_HAVOC_UF=0/1
    // overrides it in check_function.
    bool havoc_uf = true;
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
// Seconds the Houdini loop-invariant search (docs/PIR.md "Loop invariants")
// may take over a whole run, shared by the pir stage's workers. A function
// that finds it spent is not attempted (invariants_note; BOUNDED stays
// BOUNDED); one that starts with less left than its own limit gets what is
// left for its search (its final query keeps its own minimum).
struct HoudiniBudget {
    explicit HoudiniBudget(double total) : total_s(total) {}
    double total_s;
    std::atomic<int64_t> spent_ms{0};
    double left() const { return total_s - static_cast<double>(spent_ms.load()) / 1000.0; }
    void spend(double s) { spent_ms.fetch_add(static_cast<int64_t>(s * 1000.0)); }
};

// The solver time one function may take (CheckOptions::function_budget_s,
// $PRISM_FUNCTION_BUDGET; docs/PIR.md "Solving"): a clock started once and
// shared by everything checked for that function (both unwind attempts of
// check_function, and the pir stage's re-checks of the same row).
struct FunctionBudget {
    explicit FunctionBudget(double total) : total_s(total) {}
    double total_s;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double left() const {
        return total_s - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    bool exhausted() const { return left() <= 0; }
};

struct CheckOptions {
    int unwind = 8;
    // Deepening of a single still-BOUNDED loop (doubling the unwind) up to
    // this bound; -1: PRISM_PIR_DEEPEN_MAX (default 64); 0: off.
    int deepen_max = -1;
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
    // Seconds all plain solver work of one function may take together
    // (property VCs, the unwinding assertion, k-induction and Houdini; not
    // certification, which has certify_budget_s). 0: no cap beyond each VC
    // timeout. Spent before every VC is answered: TIMEOUT with message
    // "pir function budget of N s spent ... (PRISM_FUNCTION_BUDGET)" and
    // extra.function_budget_s (Law 7). Spent after that: the verdict stands
    // (a BOUNDED stays BOUNDED, a validated violation stays FAILED) and a note
    // says what was not attempted.
    double function_budget_s = 0;
    // The clock of that budget; null: check_function starts one (shared by
    // its two unwind attempts). The pir stage sets it to share one budget
    // with its re-checks of the same function.
    std::shared_ptr<FunctionBudget> function_budget;
    // Complexity limits (include/prism/complexity.hpp; docs/TARGETED_PROVING.md
    // W1): rlimit units of one VC query of a function of reference size.
    // -1: complexity::scale_for(timeout_s) (--complexity, $PRISM_PIR_RLIMIT,
    // else timeout_s x kUnitsPerSecond); 0: off (the wall-clock limits alone,
    // as before). On, every per-query time limit t (a VC's timeout_s, a
    // Houdini query's share, the Houdini search, a k-induction step) becomes
    // t x complexity / timeout_s x complexity::size_factor(function) Z3
    // rlimit units, and its wall clock a backstop at kWallBackstop x t.
    std::int64_t complexity = -1;
    // The deterministic part of the function budget, in seconds of the
    // units above (0: none, function_budget_s alone). Spent, it ends the
    // function exactly as a spent function_budget_s does (same messages,
    // "rlimit units" named), but at the same point on every machine. The pir
    // stage sets it to the function budget and function_budget_s to
    // kWallBackstop x that (the backstop).
    double function_units_s = 0;
    // --jobs of the run (W2): a function's property VCs and its strategies
    // (unwinding, k-induction, Houdini) are asked concurrently on the run's
    // shared scheduler (prism::pool) when this is above 1; the verdict is the
    // one the sequential order gives (docs/TARGETED_PROVING.md W2).
    int jobs = 1;
    // Fault injection (tests): called before each plain solver query of the
    // function (the VC label, "k-induction@k" or "houdini"), after the budget
    // check and the query's timeout are set, so a delay here stands for a
    // slow query.
    std::function<void(const std::string&)> debug_before_query;
    bool use_cache = true;
    std::string cache_dir;      // empty: the solver library's default
    unsigned max_parallel = 0;  // solver members at once; 0: hardware threads
    std::vector<std::string> tool_dirs;  // searched first (tests); see SolveOptions
    bool search_default_tools = true;
    // --tool NAME=PATH (Config::tools) and Law 9 refuse_tools_under (scanned root).
    std::map<std::string, std::string> tool_paths;
    std::string refuse_tools_under;
    EncodeOptions encode;       // memory encoding (Bv: QF_BV, certifiable)
    // Houdini: the run's shared budget (null: none, each function keeps its
    // own limit), and whether its outcome is cached under cache_dir/houdini
    // (use_cache; a cached proof is re-checked, docs/PIR.md "Loop invariants")
    std::shared_ptr<HoudiniBudget> houdini_budget;
    // Non-empty: only Check statements whose prop is listed are verified;
    // the others are dropped (not assumed). Used by the AI loop-invariant
    // checks (ai::check_program, only_invariants), never by a verdict run.
    std::vector<std::string> only_props;
};
PRISM_API Verdict check_function(const Function& fn, const CheckOptions& opt);
// True once Z3 hit its hard memory limit in this process: every later
// check_function is UNKNOWN "not checked" (pir stage re-runs those in a
// fresh process).
PRISM_API bool z3_hard_oom();
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
    std::string version;       // "clang-23" (from --version)
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
                                                 std::string* cxx_models = nullptr,
                                                 const std::vector<std::string>& include_flags = {},
                                                 int cxx_std = 17);
// The flags of `unit` from cfg.compile_commands (--compile-commands), as the
// pir stage takes them (docs/PIR.md "Units with their own flags"): empty when
// the option is unset or the file has no entry (`note` then says so). `note`
// names the entry and the flags deliberately not taken; `cxx_std` is raised
// to the entry's -std=c++NN year when there is one.
PRISM_API std::vector<std::string> unit_compile_flags(const Config& cfg, const std::filesystem::path& unit,
                                                      int* cxx_std = nullptr, std::string* note = nullptr);
// cxx_std: the C++ standard the unit is built with (its compile_commands.json
// -std=, else 17, the default of the compilers PRISM runs). Below 20 a signed
// left shift whose result does not fit is UB (C++11..17, CWG 1457), so the
// signed-shift locator also runs for C++ with that standard.
// include_flags: the unit's -I/-D/... from compile_commands.json (never -std=,
// which stays PRISM's own), or a search path guess; appended to every clang run.
// cxx_models receives which C++ library the unit was lowered with when
// fe.cxx_models is set: "" (the unit uses no modelled header), "model: vector"
// or "libstdc++ (fallback: <why>)" when the unit does not compile against the
// models (it is then lowered with the platform library, never skipped).

// ---------------------------------------------------------------------------
// Library models (roadmap 2.6, docs/PIR.md "Library models")
// ---------------------------------------------------------------------------

// Sources of the operational models, embedded in the binary at build time:
// (file name, text). Libc models (src/prism/pir/models/libc/*.cpp, prism_model.h)
// by file name; C++ library model headers as "cxx/<header>".
PRISM_API const std::vector<std::pair<std::string, std::string>>& model_sources();
// Write the C++ model headers ("cxx/..." sources) into dir; false on an I/O error.
PRISM_API bool write_cxx_models(const std::filesystem::path& dir);
// `import std;` support: true when the source text imports the std (or
// std.compat) module outside a comment or string.
PRISM_API bool source_imports_std(const std::string& text);
// Precompile the modelled std module (cxx/std.cppm, written by
// write_cxx_models into models_dir) with clangxx at the given -std, to
// out_pcm. The unit is then lowered with "-fmodule-file=std=<out_pcm>".
// false (err set) when the module does not build; the caller then falls
// back to libstdc++ as for any unit that does not compile against the models.
PRISM_API bool build_cxx_std_module(const std::filesystem::path& clangxx, const std::filesystem::path& models_dir,
                                    const std::string& cxx_std, const std::vector<std::string>& extra_flags,
                                    const std::filesystem::path& out_pcm, double timeout_s, std::string& err);

struct ModelLibrary {
    std::vector<ir::Module> units;  // lowered model files
    std::vector<std::string> unit_names;  // file name of each unit (kernel specs: linux_*.cpp, freebsd_*.cpp)
    std::string error;              // non-empty: the models could not be built
};
// Lower the embedded models with the same clang/opt pipeline (once per run).
PRISM_API ModelLibrary build_models(const Frontend& fe, double timeout_s);
// Link into m every model function m declares but does not define
// (transitively, with the globals they use). Returns the linked names.
// kernel_api "linux" / "freebsd" also links the kernel API specs of that kernel
// (models/kernel/linux_*.cpp, freebsd_*.cpp, docs/KERNEL_MODELS.md); they take
// precedence over the libc models of the same name. "" links the libc models only.
PRISM_API std::vector<std::string> link_models(ir::Module& m, const ModelLibrary& lib,
                                               const std::string& kernel_api = "");

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

// --lock-contracts FILE: read and parse (throws std::runtime_error on a missing file or a bad line)
PRISM_API std::vector<LockContract> read_lock_contracts(const std::string& path);

// Lock-order inversions (LOCK-ORDER-INVERSION, docs/KERNEL_CONCURRENCY.md section 14): a module pass
// over the unit's IR (src/prism/pir/lock_order.cpp). A class pair taken in both orders is one entry.
struct LockOrderInversion {
    std::string function;       // the function whose path takes `second` before `first`
    std::string other;          // the function whose path takes `first` before `second`
    std::string first, second;  // lock classes (module global names), first < second
};
// A cycle of three or more lock classes: classes[i] is taken before classes[i+1] (cyclically), in functions[i].
struct LockOrderCycle {
    std::vector<std::string> classes;    // rotated so that the smallest class comes first
    std::vector<std::string> functions;  // witness of each edge; functions.back() closes the cycle
};
// LOCK-CTX-MISMATCH (lockdep inconsistent {IN-HARDIRQ-W} -> {HARDIRQ-ON-W}): a spinlock class taken in an
// interrupt handler's context (hardirq: the handler and its callees to depth 3) and also taken without
// interrupt disabling in another function.
struct LockCtxMismatch {
    std::string function;    // the process-context function that takes the class without irq disabling
    std::string handler;     // the interrupt handler whose context takes the same class
    std::string lock_class;  // module global name
};
// A sleeping primitive reached in hardirq context (SLEEP-IN-ATOMIC, interrupt handler).
struct HardirqSleep {
    std::string function;  // the function whose body (or a callee's) calls the sleeping primitive
    std::string handler;   // the interrupt handler it is reached from
    std::string callee;    // the sleeping primitive
};
// What the lock-order pass saw of one unit (not findings): lock call sites and how many resolved to a class.
// acquire_sites/release_sites: calls to a lock primitive (or a contract row) in defined functions. *_reached: seen by
// the exploration; *_resolved: given a class (global, struct field, or a parameter class from a caller) in at least one
// exploration. static_kind: acquire sites by what the lock operand is ("global", "field", "param", "unresolved:...").
struct LockCoverage {
    long functions = 0, acquire_sites = 0, release_sites = 0, table_sites = 0;
    long acquire_reached = 0, acquire_resolved = 0, release_reached = 0, release_resolved = 0;
    long geps = 0, field_geps = 0, pairs = 0, steps = 0;
    bool step_limit_hit = false;
    std::map<std::string, long> static_kind;
    std::map<std::string, long> unmodelled_lock_calls;  // calls to undefined lock-like functions the pass does not model
    std::set<std::string> global_classes, field_classes;
};
struct LockOrderReport {
    LockCoverage coverage;
    std::vector<LockOrderInversion> inversions;
    std::vector<LockOrderCycle> cycles;
    std::vector<LockCtxMismatch> ctx_mismatches;
    std::vector<HardirqSleep> hardirq_sleeps;
};
// The lock-order and interrupt-context pass over the unit (docs/KERNEL_CONCURRENCY.md sections 14 and 15).
PRISM_API LockOrderReport lock_order_report(const ir::Module& m, const std::vector<LockContract>& contracts,
                                            bool scopes = false);
// --pir-lock-scopes: the locks a root's call tree acquires and releases through PRISM's lock models, by lock class (a
// global's name, followed through zero-offset getelementptrs, spinlock_check-style helpers and guard() slots): bit 0
// acquired, bit 1 released, bit 2 a spin acquire, bit 3 a spin release. unknown_*: some acquire (release) has a lock
// that is not a classified global, or the walk could not see it, so no lock may be called release-only (acquire-only).
struct LockSites {
    std::map<std::string, int> by_class;
    bool unknown_acquire = false;
    bool unknown_release = false;
    // RCU read-side nesting and preempt / bh count changes through the models: bit 0 an increment, bit 1 a decrement,
    // bit 2 a change whose sign is not known (or not seen)
    int rcu = 0;
    int preempt = 0;
};
PRISM_API LockSites lock_sites(const ir::Module& m, const ir::Function& root);
PRISM_API std::vector<LockOrderInversion> lock_order_inversions(const ir::Module& m,
                                                                const std::vector<LockContract>& contracts);

// The pir stage: one finding per defined function of every C/C++ unit.
PRISM_API std::vector<Finding> run_pir(const std::vector<std::filesystem::path>& sources,
                                       const Config& cfg);

// Tooling (prism-qa solver-bench; src/prism/pir/bench.cpp). JSON on return.
// Every VC of every encodable function of `src`, written as SMT-LIB2 files
// under out_dir; functions that are not encoded are listed with their status.
PRISM_API std::string unit_vcs_json(const std::filesystem::path& src, const Config& cfg,
                                    const std::filesystem::path& out_dir);
// One VC through prism::solver::solve (Z3 alone when z3_only; cfg.timeout,
// cfg.certified). The query cache is off (a timing tool); cfg.solver_cache
// is where the scheduler's solve-time history is kept.
PRISM_API std::string solve_smt2_json(const std::string& smt2, const Config& cfg, bool z3_only);

// The pir stage's default per-function budget in seconds when
// $PRISM_FUNCTION_BUDGET is unset: max(300, 10 x cfg.timeout).
double default_function_budget_s(const Config& cfg);

}  // namespace prism::pir
