// Differential trace of <numeric> (tests/cpp/test_cxx_numeric.cpp).
// Compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/numeric) first on the include path.
// Midpoint of a large positive pair is that value, not a negative.
// gcd(0, 0) is 0. accumulate and the other omitted algorithms are not called.
#include <climits>
#include <cstdio>
#include <numeric>

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

int main() {
    const int hi = std::midpoint(INT_MAX, INT_MAX);
    const int near = std::midpoint(INT_MAX - 2, INT_MAX);
    std::printf("hi_neg=%d hi_zero=%d hi_eq=%d\n", hi < 0, hi == 0, hi == INT_MAX);
    std::printf("near_neg=%d near_zero=%d near=%d\n", near < 0, near == 0, near);
    std::printf("small=%d\n", std::midpoint(2, 4));
    std::printf("gcd00=%d\n", std::gcd(0, 0));
    std::printf("gcd=%d\n", std::gcd(12, 18));
    std::printf("gcd0=%d\n", std::gcd(0, 15));
    std::printf("gcd_neg=%d\n", std::gcd(-4, 6));
    return 0;
}
