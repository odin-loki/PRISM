#pragma once

// Translation of LLVM memory instructions to PIR (docs/PIR.md "Memory
// model"). The core translator (translate.cpp) implements TrApi; MemTr only
// emits PIR through it, so the memory model stays in its own files.

#include "prism/pir.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace prism::pir::pirmem {

struct Unenc {
    std::string reason;
};

inline constexpr unsigned kPtrW = 64;

class TrApi {
public:
    virtual ~TrApi() = default;
    virtual const ir::Module& module() const = 0;
    virtual const TranslateOptions& options() const = 0;
    virtual Function& fn() = 0;
    virtual int newvar(const std::string& name, unsigned w) = 0;
    virtual Arg assign(int b, Op op, unsigned w, std::vector<Arg> args, const char* base = "t") = 0;
    virtual void check(int b, Arg viol, std::string prop, std::string cls, std::string msg, int line) = 0;
    virtual void assume(int b, Arg cond) = 0;
    virtual Arg havoc(int b, unsigned w, bool uninit, bool nondet, int dst = -1) = 0;
    // b == -1: the function prologue (runs first, before block 0's statements)
    virtual void push(int b, Stmt s) = 0;
};

// Target data layout (x86-64 defaults, overridden by the module's
// datalayout string: iN, f, p, a specs).
class Layout {
public:
    explicit Layout(const ir::Module& m);
    uint64_t store_size(const ir::Type& t) const;  // bytes a load/store touches
    uint64_t alloc_size(const ir::Type& t) const;  // with tail padding (arrays, alloca)
    unsigned align(const ir::Type& t) const;       // ABI alignment
    uint64_t field_offset(const ir::Type& st, unsigned idx) const;
    const ir::Type& resolve(const ir::Type& t) const;  // named struct -> body
    const ir::Type& field_type(const ir::Type& st, unsigned idx) const;

private:
    const ir::Module& m_;
    std::map<unsigned, unsigned> int_align_;  // bits -> bytes
    unsigned ptr_bytes_ = 8, ptr_align_ = 8;
};

class MemTr {
public:
    MemTr(TrApi& t, const ir::Module& m);

    const Layout& layout() const { return lay_; }
    // Width of a first-class value type (iN <= 64, ptr = 64); throws Unenc.
    unsigned value_width(const ir::Type& t, std::string_view what) const;

    Arg global(const std::string& name);  // pointer to @name, allocated in the prologue
    Arg const_gep(int b, const ir::Value& ce, int line);  // constant getelementptr expression

    // alloca: dst gets a fresh stack object. count: element count (dynamic
    // allocas / VLAs), count_src: the value before its zext/sext (VLA size
    // positivity is checked on it).
    void alloca_(int b, int dst, const ir::Type& ety, unsigned align, std::optional<Arg> count,
                 std::optional<Arg> count_src, const std::string& name, int line);
    // Returns the "some byte uninitialised" shadow variable. check_uninit =
    // false: the caller tracks the shadow to the value's uses instead (a raw
    // byte copy such as an ABI-coerced struct load).
    // raw = true: no check, and the shadow is a per-byte mask (bit k = byte k uninitialised).
    int load(int b, int dst, const ir::Type& ty, Arg ptr, unsigned align, int line, bool check_uninit = true);
    void store(int b, Arg ptr, Arg val, Arg init, const ir::Type& ty, unsigned align, int line);
    // Array indices must stay in their array (< N when the result is
    // dereferenced, <= N for an address). use: 0 address only, 1 loaded,
    // 2 stored through, 3 base of a further getelementptr. base_last_field:
    // the base is the last field of a struct (flexible-array idiom: exempt).
    void gep(int b, int dst, Arg base, const ir::Type& ety, const std::vector<Arg>& idx, bool inbounds, int line,
             int use = 3, bool base_last_field = false);
    void icmp(int b, int dst, const std::string& pred, Arg x, Arg y, int line);
    void ptr_sub_check(int b, Arg x, Arg y, int line);
    void memcpy_(int b, Arg dst, Arg src, Arg len, bool move, int line);
    void memset_(int b, Arg dst, Arg byte, Arg len, int line);
    // heap / new / FILE: returns the pointer (never null here; the models decide failure)
    Arg alloc(int b, Arg size, MemKind k, int init, const std::string& what, int line, int dst = -1);
    void dealloc(int b, Arg ptr, MemKind expected, const std::string& what, int line, bool kill = true);
    void end_lifetime(int b, Arg ptr);  // stack object of an inlined callee at its return
    void stack_escape_check(int b, Arg ret, const std::vector<Arg>& own, int line);
    Arg obj_size_remaining(int b, Arg ptr);  // size - offset (0 when out of range)
    void read_range(int b, Arg ptr, Arg len, int line);  // range access checks (read)
    void write_havoc(int b, Arg ptr, Arg len, int line);  // range written with arbitrary bytes
    Arg fresh_cstr(int b, MemKind k, int line);
    Arg stack_save(int b);
    void stack_restore(int b, Arg token);
    // Constant C string at a pointer operand (@.str / constant GEP into one).
    std::optional<std::string> const_string(const ir::Value& v) const;

private:
    TrApi& t_;
    Layout lay_;
    std::map<std::string, Arg> globals_;

    Arg c64(uint64_t v) const { return Arg::c(64, v); }
    Arg obj(int b, Arg p);
    Arg off(int b, Arg p);
    Arg p2(int b, Op op, Arg x, Arg y) { return t_.assign(b, op, 1, {x, y}, "c"); }
    Arg band(int b, Arg x, Arg y) { return t_.assign(b, Op::And, 1, {x, y}, "c"); }
    Arg bor(int b, Arg x, Arg y) { return t_.assign(b, Op::Or, 1, {x, y}, "c"); }
    Arg ptr_add(int b, Arg p, Arg delta, int dst = -1);
    void access_checks(int b, Arg p, Arg n, bool write, unsigned align, int line, std::optional<Arg> guard = {});
    void emit_init(Arg base, uint64_t off, const ir::Type& ty, const ir::Value& v, const std::string& gname);
    void mark_memory() { t_.fn().uses_memory = true; }
    unsigned tag_of(const ir::Type& ty) const;
};

// printf family (libc_format.cpp): what a call with a literal format needs.
struct FormatPlan {
    bool handled = false;        // callee is a modelled format function
    std::string unencoded;       // non-empty: "UNENCODED: ..." (non-literal format, va_list, ...)
    int fmt_arg = -1;
    int buf_arg = -1;            // sprintf / snprintf destination
    int size_arg = -1;           // snprintf size
    uint64_t max_len = 0;        // sprintf: upper bound of the output length (without NUL)
    std::vector<std::pair<std::string, std::string>> violations;  // (class, message), always reached
    std::vector<std::size_t> cstr_args;  // %s arguments (NUL-terminated string required)
};
// callee: symbol; fmt: the literal format when known; args: IR types of all call arguments.
PRISM_API FormatPlan plan_format(const std::string& callee, const std::optional<std::string>& fmt,
                                 const std::vector<ir::Type>& args);
PRISM_API bool is_format_function(const std::string& callee);

}  // namespace prism::pir::pirmem
