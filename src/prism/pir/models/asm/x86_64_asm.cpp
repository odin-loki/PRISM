// Reference semantics and table for x86-64 inline-asm templates (see x86_64_asm.h).
#include "x86_64_asm.h"

#include <array>

namespace prism::pir::asmodel {

namespace {

constexpr std::array<X86Model, 13> kTable{{
    {"pause", AsmClass::Fence,
     "no register or memory effect; a spin-wait hint", "none", true},
    {"mfence", AsmClass::Fence,
     "full memory fence: all earlier loads and stores globally visible before later ones",
     "ordering only; no register, no memory value", true},
    {"lfence", AsmClass::Fence,
     "load fence: earlier loads complete before later instructions", "ordering only", true},
    {"sfence", AsmClass::Fence,
     "store fence: earlier stores globally visible before later stores", "ordering only", true},
    {"bswap %0", AsmClass::Pure,
     "%0 := byte-reversed %0 (64-bit)", "flags unchanged", true},
    {"lock; xaddq %0, %1", AsmClass::Memory,
     "atomic: tmp = mem; mem = tmp + reg; reg = tmp", "flags clobbered (not modelled); memory RMW", true},
    {"lock; cmpxchgq %2, %1", AsmClass::Memory,
     "atomic: if mem == rax then mem = src, ZF = 1 else rax = mem, ZF = 0",
     "flags: ZF modelled, others clobbered; memory RMW", true},
    {"xchgq %0, %1", AsmClass::Memory,
     "atomic (implicit lock): swap register and memory", "no flags; memory RMW", true},
    {"lock; addq %1, %0", AsmClass::Memory,
     "atomic: mem = mem + src; no register output", "flags clobbered (not modelled); memory RMW", true},
    {"rdtsc", AsmClass::Nondet,
     "edx:eax := time-stamp counter; monotonic per CPU, value not modelled",
     "machine value; validated only as monotonic", true},
    {"cpuid", AsmClass::Nondet,
     "eax..edx := CPU identification for leaf eax and subleaf ecx; not modelled",
     "machine-specific; never faults; not compared", false},
    {"rdmsr", AsmClass::Privileged,
     "edx:eax := MSR[ecx]; #GP for unknown MSR (not modelled)",
     "state/system effect; needs ring 0; UNVALIDATED", false},
    {"wrmsr", AsmClass::Privileged,
     "MSR[ecx] := edx:eax; system state effect", "state/system effect; needs ring 0; UNVALIDATED", false},
}};

}  // namespace

std::optional<X86Model> find_x86_model(std::string_view tmpl) {
    for (const auto& m : kTable)
        if (m.tmpl == tmpl) return m;
    return std::nullopt;
}

std::size_t x86_model_count() { return kTable.size(); }

std::uint64_t bswap64(std::uint64_t v) {
    std::uint64_t r = 0;
    for (int i = 0; i < 8; ++i) {
        r = (r << 8) | (v & 0xff);
        v >>= 8;
    }
    return r;
}

XaddOut lock_xadd64(std::uint64_t mem, std::uint64_t src) {
    return {mem + src, mem};
}

CmpxchgOut lock_cmpxchg64(std::uint64_t mem, std::uint64_t rax, std::uint64_t src) {
    if (mem == rax) return {src, rax, true};
    return {mem, mem, false};
}

XchgOut xchg64(std::uint64_t mem, std::uint64_t reg) {
    return {reg, mem};
}

std::uint64_t lock_add64(std::uint64_t mem, std::uint64_t src) {
    return mem + src;
}

}  // namespace prism::pir::asmodel
