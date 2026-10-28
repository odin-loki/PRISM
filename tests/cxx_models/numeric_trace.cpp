// Differential trace of <numeric> (tests/cpp/test_cxx_numeric.cpp).
// Compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/numeric) first on the include path.
// Midpoint of a large positive pair is that value, not a negative.
// gcd(0, 0) is 0. lcm(0, x), lcm(x, 0), and lcm(0, 0) are 0.
// argv "zero" is lcm(0, INT_MIN), which is 0. argv "overflow" is a product
// that does not fit: the model must __prism_check and must not print a
// wrapped value or 0. accumulate and the other omitted algorithms are not called.
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>

// The model declares this PIR intrinsic and does not define it. libstdc++
// <numeric> does not call it. A failing check aborts before a wrapped value
// or a 0 from a nonzero pair can be printed.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

static_assert(std::midpoint(2, 4) == 3);
static_assert(std::midpoint(4, 2) == 3);
static_assert(std::midpoint(1, 2) == 1);
static_assert(std::midpoint(2, 1) == 2);
static_assert(std::midpoint(INT_MAX, INT_MAX) == INT_MAX);
static_assert(std::midpoint(INT_MAX, INT_MAX) > 0);
static_assert(std::midpoint(INT_MAX - 2, INT_MAX) == INT_MAX - 1);
static_assert(std::midpoint(INT_MIN, INT_MIN) == INT_MIN);
static_assert(std::midpoint(INT_MIN, INT_MAX) == -1);
static_assert(std::gcd(0, 0) == 0);
static_assert(std::gcd(12, 18) == 6);
static_assert(std::gcd(0, 15) == 15);
static_assert(std::gcd(15, 0) == 15);
static_assert(std::gcd(-4, 6) == 2);
static_assert(std::lcm(0, 0) == 0);
static_assert(std::lcm(0, 15) == 0);
static_assert(std::lcm(15, 0) == 0);
static_assert(std::lcm(12, 18) == 36);
static_assert(std::lcm(-4, 6) == 12);
static_assert(std::lcm(4, -6) == 12);
static_assert(std::lcm(-4, -6) == 12);
static_assert(std::lcm(7, 7) == 7);
static_assert(std::lcm(INT_MAX, 1) == INT_MAX);
static_assert(std::lcm(1, INT_MAX) == INT_MAX);
static_assert(std::lcm(INT_MAX, INT_MAX) == INT_MAX);
static_assert(std::lcm(INT_MAX, 2LL) == static_cast<long long>(INT_MAX) * 2);
static_assert(std::lcm(0u, 15u) == 0u);
static_assert(std::lcm(15u, 0u) == 0u);
static_assert(std::lcm(12u, 18u) == 36u);
static_assert(std::lcm(-6, 8u) == 24u);

static bool arg_is(int argc, char** argv, const char* want) {
    if (argc < 2 || argv[1] == nullptr) return false;
    return std::strcmp(argv[1], want) == 0;
}

int main(int argc, char** argv) {
    if (arg_is(argc, argv, "zero")) {
        volatile int z = 0;
        volatile int m = INT_MIN;
        const int a = std::lcm(z, m);
        const int b = std::lcm(m, z);
        const int c = std::lcm(z, z);
        std::printf("zero_min=%d min_zero=%d zero_zero=%d\n", a, b, c);
        return 0;
    }
    if (arg_is(argc, argv, "overflow")) {
        volatile int m = INT_MIN;
        volatile int one = 1;
        const int min1 = std::lcm(m, one);
        std::printf("wrapped=%d\n", min1);
        volatile int a = INT_MAX;
        volatile int b = 2;
        const int r = std::lcm(a, b);
        std::printf("wrapped=%d\n", r);
        volatile unsigned u = ~0u;
        volatile unsigned v = ~0u - 1u;
        const unsigned ur = std::lcm(u, v);
        std::printf("wrapped_u=%u\n", ur);
        return (min1 == 0 || r == 0 || ur == 0) ? 2 : 0;
    }
    const int hi = std::midpoint(INT_MAX, INT_MAX);
    const int near = std::midpoint(INT_MAX - 2, INT_MAX);
    std::printf("hi_neg=%d hi_zero=%d hi_eq=%d\n", hi < 0, hi == 0, hi == INT_MAX);
    std::printf("near_neg=%d near_zero=%d near=%d\n", near < 0, near == 0, near);
    std::printf("small=%d\n", std::midpoint(2, 4));
    std::printf("gcd00=%d\n", std::gcd(0, 0));
    std::printf("gcd=%d\n", std::gcd(12, 18));
    std::printf("gcd0=%d\n", std::gcd(0, 15));
    std::printf("gcd_neg=%d\n", std::gcd(-4, 6));
    std::printf("lcm00=%d\n", std::lcm(0, 0));
    std::printf("lcm0=%d\n", std::lcm(0, 15));
    std::printf("lcm_x0=%d\n", std::lcm(15, 0));
    std::printf("lcm=%d\n", std::lcm(12, 18));
    std::printf("lcm_neg=%d\n", std::lcm(-4, 6));
    std::printf("lcm_both=%d\n", std::lcm(-4, -6));
    std::printf("lcm_max=%d\n", std::lcm(INT_MAX, 1));
    std::printf("lcm_ll=%lld\n", static_cast<long long>(std::lcm(INT_MAX, 2LL)));
    std::printf("lcm_u=%u\n", std::lcm(12u, 18u));
    std::printf("lcm_mix=%u\n", std::lcm(-6, 8u));
    return 0;
}
