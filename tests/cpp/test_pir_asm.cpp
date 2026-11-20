// x86-64 inline-asm semantics (W4, src/prism/pir/models/asm/x86_64_asm.h): every
// validated template agrees with native execution of the same instruction on
// random vectors (at least 1,000 each). Privileged and machine-specific entries
// stay UNVALIDATED and are not executed. Only built and run on x86-64 with a GNU-
// style inline asm (clang or gcc); elsewhere the test is a no-op.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "../../src/prism/pir/models/asm/x86_64_asm.h"

#include <cstdint>
#include <random>

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
using namespace prism::pir::asmodel;

namespace {

constexpr int kVectors = 2048;  // at least 1,000 per instruction

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

}  // namespace

TEST_CASE("asm: x86-64 bswap agrees with native execution") {
    std::mt19937_64 rng(20261116);
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t v = rng();
        CHECK(native_bswap(v) == bswap64(v));
    }
}

TEST_CASE("asm: x86-64 lock xadd, xchg and lock add agree with native execution") {
    std::mt19937_64 rng(20261117);
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t mem = rng(), src = rng();
        XaddOut n = native_xadd(mem, src), m = lock_xadd64(mem, src);
        CHECK(n.mem == m.mem);
        CHECK(n.reg == m.reg);
        XchgOut xn = native_xchg(mem, src), xm = xchg64(mem, src);
        CHECK(xn.mem == xm.mem);
        CHECK(xn.reg == xm.reg);
        CHECK(native_lock_add(mem, src) == lock_add64(mem, src));
    }
}

TEST_CASE("asm: x86-64 lock cmpxchg agrees with native execution, ZF included") {
    std::mt19937_64 rng(20261118);
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t mem = rng(), rax = (i & 1) ? mem : rng(), src = rng();
        CmpxchgOut n = native_cmpxchg(mem, rax, src), m = lock_cmpxchg64(mem, rax, src);
        CHECK(n.mem == m.mem);
        CHECK(n.rax == m.rax);
        CHECK(n.zf == m.zf);
    }
}

TEST_CASE("asm: x86-64 fences leave memory unchanged") {
    std::mt19937_64 rng(20261119);
    for (int i = 0; i < kVectors; ++i) {
        volatile std::uint64_t word = rng();
        std::uint64_t before = word;
        asm volatile("pause" ::: "memory");
        asm volatile("mfence" ::: "memory");
        asm volatile("lfence" ::: "memory");
        asm volatile("sfence" ::: "memory");
        CHECK(word == before);
    }
}

TEST_CASE("asm: x86-64 rdtsc reads never decrease") {
    std::uint64_t prev = native_rdtsc();
    for (int i = 0; i < kVectors; ++i) {
        std::uint64_t now = native_rdtsc();
        CHECK(now >= prev);
        prev = now;
    }
}

TEST_CASE("asm: x86-64 table: validated entries and unvalidated privileged ones") {
    CHECK(find_x86_model("lock; xaddq %0, %1").has_value());
    CHECK(find_x86_model("lock; xaddq %0, %1")->validated);
    auto rdmsr = find_x86_model("rdmsr");
    REQUIRE(rdmsr.has_value());
    CHECK(rdmsr->cls == AsmClass::Privileged);
    CHECK_FALSE(rdmsr->validated);
    CHECK_FALSE(find_x86_model("no such template").has_value());
}

#else

TEST_CASE("asm: x86-64 semantics not built on this target") {
    CHECK(find_x86_model("pause").has_value());
}

#endif
