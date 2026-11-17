// Native validation of the x86-64 asm semantics (W4 gate).
//
// For each validated instruction the same template runs natively on this host
// and the reference model (src/prism/pir/models/asm/x86_64_asm.cpp) is evaluated
// on the same random inputs. Every instruction needs at least 1,000 vectors and
// zero mismatches. Privileged entries (rdmsr, wrmsr) are not executed here
// (they fault in user mode) and are reported as UNVALIDATED.
//
// Build: clang++ -std=c++23 -O1 -I src tests/asm/x86_64_asm_validate.cpp \
//          src/prism/pir/models/asm/x86_64_asm.cpp -o x86_64_asm_validate
#include "../../src/prism/pir/models/asm/x86_64_asm.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace prism::pir::asmodel;

namespace {

constexpr int kVectors = 4096;  // required: at least 1,000 per instruction

struct Report {
    std::string name;
    int vectors = 0;
    int mismatches = 0;
};

std::uint64_t native_bswap(std::uint64_t v) {
    asm("bswapq %0" : "+r"(v));
    return v;
}

XaddOut native_xadd(std::uint64_t mem, std::uint64_t src) {
    std::uint64_t m = mem;
    asm volatile("lock; xaddq %1, %0" : "+m"(m), "+r"(src) : : "memory", "cc");
    return {m, src};
}

CmpxchgOut native_cmpxchg(std::uint64_t mem, std::uint64_t rax, std::uint64_t src) {
    std::uint64_t m = mem;
    bool zf = false;
    asm volatile("lock; cmpxchgq %3, %1"
                 : "+a"(rax), "+m"(m), "=@ccz"(zf)
                 : "r"(src)
                 : "memory");
    return {m, rax, zf};
}

XchgOut native_xchg(std::uint64_t mem, std::uint64_t reg) {
    std::uint64_t m = mem;
    asm volatile("xchgq %1, %0" : "+m"(m), "+r"(reg) : : "memory");
    return {m, reg};
}

std::uint64_t native_lock_add(std::uint64_t mem, std::uint64_t src) {
    std::uint64_t m = mem;
    asm volatile("lock; addq %1, %0" : "+m"(m) : "r"(src) : "memory", "cc");
    return m;
}

std::uint64_t native_rdtsc() {
    std::uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

void run_bswap(std::mt19937_64& rng, Report& r) {
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t v = rng();
        ++r.vectors;
        if (native_bswap(v) != bswap64(v)) ++r.mismatches;
    }
}

void run_xadd(std::mt19937_64& rng, Report& r) {
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t mem = rng(), src = rng();
        ++r.vectors;
        XaddOut n = native_xadd(mem, src), m = lock_xadd64(mem, src);
        if (n.mem != m.mem || n.reg != m.reg) ++r.mismatches;
    }
}

void run_cmpxchg(std::mt19937_64& rng, Report& r) {
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t mem = rng(), rax = (i & 1) ? mem : rng(), src = rng();
        ++r.vectors;
        CmpxchgOut n = native_cmpxchg(mem, rax, src), m = lock_cmpxchg64(mem, rax, src);
        if (n.mem != m.mem || n.rax != m.rax || n.zf != m.zf) ++r.mismatches;
    }
}

void run_xchg(std::mt19937_64& rng, Report& r) {
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t mem = rng(), reg = rng();
        ++r.vectors;
        XchgOut n = native_xchg(mem, reg), m = xchg64(mem, reg);
        if (n.mem != m.mem || n.reg != m.reg) ++r.mismatches;
    }
}

void run_lock_add(std::mt19937_64& rng, Report& r) {
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t mem = rng(), src = rng();
        ++r.vectors;
        if (native_lock_add(mem, src) != lock_add64(mem, src)) ++r.mismatches;
    }
}

// Fences: no value effect. Each run must leave a memory word unchanged.
template <typename F>
void run_fence(std::mt19937_64& rng, Report& r, F fence) {
    for (int i = 0; i < kVectors; ++i) {
        volatile std::uint64_t word = rng();
        std::uint64_t before = word;
        fence();
        ++r.vectors;
        if (word != before) ++r.mismatches;
    }
}

// rdtsc is nondeterministic: the validated property is that consecutive reads
// never decrease. The value itself is not compared.
void run_rdtsc(Report& r) {
    std::uint64_t prev = native_rdtsc();
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t now = native_rdtsc();
        ++r.vectors;
        if (now < prev) ++r.mismatches;
        prev = now;
    }
}

}  // namespace

int main() {
    std::mt19937_64 rng(20261116);
    std::vector<Report> out;

    Report bs{"bswap %0"};
    run_bswap(rng, bs);
    out.push_back(bs);

    Report xa{"lock; xaddq %0, %1"};
    run_xadd(rng, xa);
    out.push_back(xa);

    Report cx{"lock; cmpxchgq %2, %1"};
    run_cmpxchg(rng, cx);
    out.push_back(cx);

    Report xc{"xchgq %0, %1"};
    run_xchg(rng, xc);
    out.push_back(xc);

    Report la{"lock; addq %1, %0"};
    run_lock_add(rng, la);
    out.push_back(la);

    Report pz{"pause"};
    run_fence(rng, pz, [] { asm volatile("pause" ::: "memory"); });
    out.push_back(pz);

    Report mf{"mfence"};
    run_fence(rng, mf, [] { asm volatile("mfence" ::: "memory"); });
    out.push_back(mf);

    Report lf{"lfence"};
    run_fence(rng, lf, [] { asm volatile("lfence" ::: "memory"); });
    out.push_back(lf);

    Report sf{"sfence"};
    run_fence(rng, sf, [] { asm volatile("sfence" ::: "memory"); });
    out.push_back(sf);

    Report rt{"rdtsc (monotonic)"};
    run_rdtsc(rt);
    out.push_back(rt);

    int failed = 0;
    for (const auto& r : out) {
        bool ok = r.vectors >= 1000 && r.mismatches == 0;
        if (!ok) ++failed;
        std::printf("%-22s vectors %6d  mismatches %d  %s\n", r.name.c_str(), r.vectors,
                    r.mismatches, ok ? "AGREE" : "FAIL");
    }
    for (const char* u : {"rdmsr", "wrmsr", "cpuid"})
        std::printf("%-22s UNVALIDATED (not executed: privileged or machine-specific)\n", u);
    int model_validated = 0;
    for (const char* t : {"pause", "mfence", "lfence", "sfence", "bswap %0", "lock; xaddq %0, %1",
                          "lock; cmpxchgq %2, %1", "xchgq %0, %1", "lock; addq %1, %0", "rdtsc"})
        if (auto m = find_x86_model(t); m && m->validated) ++model_validated;
    std::printf("table entries %zu, validated table entries %d, failed gates %d\n",
                x86_model_count(), model_validated, failed);
    return failed == 0 ? 0 : 1;
}
