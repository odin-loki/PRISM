// Instruction semantics for x86-64 inline-asm templates (workstream W4).
//
// Each entry is one normalised template (the LLVM asm string, e.g. "pause" or
// "lock; xaddq %0, %1") with its class, its declared effects, and the exact
// semantics of the register and memory effects that the translator may use
// instead of havoc. Anything not in the table stays havoc (see find_x86_model).
//
// The evaluators below are the reference semantics. tests/asm/x86_64_asm_validate.cpp
// compares them with the same instruction run natively on the host (random
// vectors); an entry is `validated` only when that comparison covers it.
// Flags are not part of the model: the effect is listed as "flags clobbered".
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::pir::asmodel {

enum class AsmClass : std::uint8_t {
    Pure,        // register inputs only, register outputs, no memory effect
    Memory,      // reads and/or writes one memory operand (atomic RMW when locked)
    Fence,       // ordering only, no value effect
    Nondet,      // outputs are a machine value (timestamp, CPU identification)
    Privileged,  // system register, port, MSR or device effect: UNVALIDATED
};

struct X86Model {
    std::string_view tmpl;       // normalised template text
    AsmClass cls;
    std::string_view semantics;  // one-line exact semantics
    std::string_view effects;    // declared effects (flags, memory, ordering)
    bool validated;              // covered by the native random-vector gate
};

// The table entry for a normalised template, or nullopt (the caller keeps havoc).
std::optional<X86Model> find_x86_model(std::string_view tmpl);

// Number of table entries (for the census and the gate report).
std::size_t x86_model_count();

// ---- LLVM asm template parsing (used by the translator, translate_asm.inc) ----

// One instruction of an LLVM inline-asm template ("lock; xaddq $0, $1").
struct X86Ins {
    std::string mn;                // mnemonic, lower case, without the lock prefix
    bool lock = false;             // lock prefix present
    std::vector<std::string> ops;  // operand texts in AT&T order (src first): "$3", "%cl"
};

// Split a template into instructions (separators: ';' and newline, including
// the escaped 0A and 09 forms the LLVM text may carry). nullopt when a piece is
// not "[lock] mnemonic op, op" (prefixes other than lock, labels, directives,
// parentheses and dialect alternatives are all "not parsed": the caller keeps
// the havoc model).
std::optional<std::vector<X86Ins>> parse_x86_template(std::string_view tmpl);

// True for an instruction that needs ring 0 or touches system state or devices
// (rdmsr, wrmsr, in, out, cli, sti, hlt, cpuid, control-register moves, ...):
// these stay havoc under the named assumption asm-privileged.
bool x86_is_privileged(const X86Ins& ins);

// The first privileged mnemonic in a template (tolerant split, also when the template does not parse).
std::optional<std::string> x86_privileged_in(std::string_view tmpl);

// The privileged mnemonics, for the census listing.
const std::vector<std::string_view>& x86_privileged_list();

// Reference semantics. All operands are 64-bit (the "q" forms).

// bswapq: byte order reversed.
std::uint64_t bswap64(std::uint64_t v);

// lock xaddq src, mem: old = mem; mem = old + src; src = old.
struct XaddOut {
    std::uint64_t mem;
    std::uint64_t reg;  // the old memory value
};
XaddOut lock_xadd64(std::uint64_t mem, std::uint64_t src);

// lock cmpxchgq src, mem (rax is the expected value): if mem == rax then
// mem = src, ZF = 1; else rax = mem, ZF = 0.
struct CmpxchgOut {
    std::uint64_t mem;
    std::uint64_t rax;
    bool zf;
};
CmpxchgOut lock_cmpxchg64(std::uint64_t mem, std::uint64_t rax, std::uint64_t src);

// xchgq reg, mem (implicitly locked): mem = reg, reg = old mem.
struct XchgOut {
    std::uint64_t mem;
    std::uint64_t reg;
};
XchgOut xchg64(std::uint64_t mem, std::uint64_t reg);

// lock addq src, mem: mem = mem + src (no register output).
std::uint64_t lock_add64(std::uint64_t mem, std::uint64_t src);

}  // namespace prism::pir::asmodel
