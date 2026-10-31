// The C23 <stdbit.h> model (src/prism/pir/models/libc/c23/stdbit.h) agrees
// with a bit-by-bit reference for every 8- and 16-bit value and for 32/64-bit
// edge values, and is found by the PIR frontend after the system headers.
#include <doctest/doctest.h>

#include "prism/pir/frontend.hpp"

#include <cstdint>
#include <string>
#include <vector>

#define _Bool bool
extern "C" {
#include "../../src/prism/pir/models/libc/c23/stdbit.h"
}
#undef _Bool

namespace {
using u8_t = unsigned char;
using u16_t = unsigned short;
using u32_t = unsigned int;
using ul_t = unsigned long;
using ull_t = unsigned long long;
template <class T>
struct Ref {
    static constexpr unsigned W = sizeof(T) * 8;
    static unsigned lz(T x) { unsigned n = 0; for (int i = W - 1; i >= 0 && !((x >> i) & 1); --i) ++n; return n; }
    static unsigned tz(T x) { unsigned n = 0; for (unsigned i = 0; i < W && !((x >> i) & 1); ++i) ++n; return n; }
    static unsigned pop(T x) { unsigned n = 0; for (unsigned i = 0; i < W; ++i) n += (x >> i) & 1; return n; }
    static unsigned width(T x) { return W - lz(x); }
    static T floor(T x) { return x ? T(T(1) << (width(x) - 1)) : T(0); }
    static T ceil(T x) {
        for (unsigned i = 0; i < W; ++i) if ((T(1) << i) >= x) return T(T(1) << i);
        return T(0);
    }
};

#define CHECK_ALL(S, T, x)                                                         \
    do {                                                                           \
        using R = Ref<T>;                                                          \
        CHECK(stdc_leading_zeros_##S(x) == R::lz(x));                              \
        CHECK(stdc_leading_ones_##S(x) == R::lz(T(~x)));                           \
        CHECK(stdc_trailing_zeros_##S(x) == R::tz(x));                             \
        CHECK(stdc_trailing_ones_##S(x) == R::tz(T(~x)));                          \
        CHECK(stdc_first_leading_one_##S(x) == (x ? R::lz(x) + 1 : 0u));           \
        CHECK(stdc_first_leading_zero_##S(x) == (T(~x) ? R::lz(T(~x)) + 1 : 0u));  \
        CHECK(stdc_first_trailing_one_##S(x) == (x ? R::tz(x) + 1 : 0u));          \
        CHECK(stdc_first_trailing_zero_##S(x) == (T(~x) ? R::tz(T(~x)) + 1 : 0u)); \
        CHECK(stdc_count_ones_##S(x) == R::pop(x));                                \
        CHECK(stdc_count_zeros_##S(x) == R::W - R::pop(x));                        \
        CHECK(stdc_has_single_bit_##S(x) == (R::pop(x) == 1));                     \
        CHECK(stdc_bit_width_##S(x) == R::width(x));                               \
        CHECK(stdc_bit_floor_##S(x) == R::floor(x));                               \
        CHECK(stdc_bit_ceil_##S(x) == R::ceil(x));                                 \
    } while (0)
}  // namespace

TEST_CASE("c23 stdbit model: matches the reference on every 8/16-bit value and 32/64-bit edges") {
    for (unsigned v = 0; v < 256; ++v) { u8_t x = (u8_t)v; CHECK_ALL(uc, u8_t, x); }
    for (unsigned v = 0; v < 65536; v += 1) { u16_t x = (u16_t)v; CHECK_ALL(us, u16_t, x); }
    std::vector<unsigned long long> e = {0, 1, 2, 3, 0x7fffffffULL, 0x80000000ULL, 0x80000001ULL, 0xffffffffULL,
                                         0x100000000ULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL,
                                         0x8000000000000001ULL, ~0ULL, 12345678901234ULL};
    for (auto v : e) {
        unsigned int x32 = (unsigned int)v;
        CHECK_ALL(ui, u32_t, x32);
        unsigned long xl = (unsigned long)v;
        CHECK_ALL(ul, ul_t, xl);
        unsigned long long xll = v;
        CHECK_ALL(ull, ull_t, xll);
    }
}

TEST_CASE("c23 stdbit model: the frontend searches it after the system headers") {
    auto f = prism::pir::lp64_clang_driver_flags();
    bool seen = false;
    for (size_t i = 0; i + 1 < f.size(); ++i)
        if (f[i] == "-idirafter" && f[i + 1].find("c23") != std::string::npos) seen = true;
    CHECK(seen);
}
