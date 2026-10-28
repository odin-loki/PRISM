// Differential trace of <numeric> (tests/cpp/test_cxx_numeric.cpp).
// Compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/numeric) first on the include path.
// Midpoint of a large positive pair is that value, not a negative.
// gcd(0, 0) is 0. lcm(0, x), lcm(x, 0), and lcm(0, 0) are 0.
// argv "zero" is lcm(0, INT_MIN), which is 0. argv "overflow" is a product
// that does not fit: the model must __prism_check and must not print a
// wrapped value or 0. argv "iota-ovf", "iota-llovf", "iota-uovf", and
// "iota-inv" are checks, not a wrapped store. argv "iota-max1" stores one
// maximum value and writes nothing further. argv "acc-ovf", "acc-llovf",
// "acc-minovf", and "acc-sc" are signed sums that do not fit. argv
// "acc-uovf" is an unsigned sum that does not fit. argv "acc-part" overflows
// on the second element. argv "acc-inv" is an inverted range. Those paths
// are checks, not a wrapped return. argv "acc-max1" adds zero to INT_MAX.
// argv "adj-ovf", "adj-llovf", "adj-minovf", "adj-sc", and "adj-part" are
// signed differences that do not fit. argv "adj-uovf" is an unsigned
// difference that does not fit. argv "adj-inv" is an inverted range. argv
// "adj-null" is a null destination. argv "adj-over" is a destination inside
// the input range. Those paths are checks, not a wrapped store. argv
// "adj-max1" copies one maximum value and writes nothing further.
// argv "ps-ovf", "ps-llovf", "ps-minovf", "ps-sc", and "ps-part" are signed
// sums that do not fit. argv "ps-uovf" and "ps-ucovf" are unsigned sums that
// do not fit. argv "ps-inv" is an inverted range. argv "ps-null" is a null
// destination. argv "ps-over" is a destination inside the input range. Those
// paths are checks, not a wrapped store. argv "ps-max1" copies one maximum
// value and writes nothing further.
// reduce, transform_reduce, inner_product, and exclusive_scan are not called.
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

constexpr bool prism_iota_seq() {
    int a[4] = {9, 9, 9, 9};
    std::iota(a, a + 4, 0);
    return a[0] == 0 && a[1] == 1 && a[2] == 2 && a[3] == 3;
}
static_assert(prism_iota_seq());

constexpr bool prism_iota_empty() {
    int a[1] = {7};
    std::iota(a, a, 1);
    return a[0] == 7;
}
static_assert(prism_iota_empty());

constexpr bool prism_iota_neg() {
    int a[3] = {};
    std::iota(a, a + 3, -1);
    return a[0] == -1 && a[1] == 0 && a[2] == 1;
}
static_assert(prism_iota_neg());

constexpr bool prism_iota_sc() {
    signed char a[2] = {};
    std::iota(a, a + 2, static_cast<signed char>(SCHAR_MAX - 2));
    return a[0] == static_cast<signed char>(SCHAR_MAX - 2) &&
           a[1] == static_cast<signed char>(SCHAR_MAX - 1);
}
static_assert(prism_iota_sc());

constexpr bool prism_iota_uc() {
    unsigned char a[2] = {};
    std::iota(a, a + 2, static_cast<unsigned char>(UCHAR_MAX - 1));
    return a[0] == static_cast<unsigned char>(UCHAR_MAX - 1) && a[1] == static_cast<unsigned char>(UCHAR_MAX);
}
static_assert(prism_iota_uc());

constexpr bool prism_iota_f() {
    float a[3] = {};
    std::iota(a, a + 3, 0.0f);
    return a[0] == 0.0f && a[1] == 1.0f && a[2] == 2.0f;
}
static_assert(prism_iota_f());

constexpr bool prism_acc_seq() {
    int a[4] = {1, 2, 3, 4};
    return std::accumulate(a, a + 4, 0) == 10;
}
static_assert(prism_acc_seq());

constexpr bool prism_acc_empty() {
    int a[1] = {7};
    return std::accumulate(a, a, 3) == 3 && a[0] == 7;
}
static_assert(prism_acc_empty());

constexpr bool prism_acc_neg() {
    int a[3] = {-1, 0, 1};
    return std::accumulate(a, a + 3, 0) == 0;
}
static_assert(prism_acc_neg());

constexpr bool prism_acc_sc() {
    signed char a[2] = {1, 1};
    return std::accumulate(a, a + 2, static_cast<signed char>(SCHAR_MAX - 2)) ==
           static_cast<signed char>(SCHAR_MAX);
}
static_assert(prism_acc_sc());

constexpr bool prism_acc_uc() {
    unsigned char a[2] = {1, 0};
    return std::accumulate(a, a + 2, static_cast<unsigned char>(UCHAR_MAX - 1)) ==
           static_cast<unsigned char>(UCHAR_MAX);
}
static_assert(prism_acc_uc());

constexpr bool prism_acc_f() {
    float a[3] = {0.0f, 1.0f, 1.0f};
    return std::accumulate(a, a + 3, 0.0f) == 2.0f;
}
static_assert(prism_acc_f());

constexpr bool prism_acc_hi() {
    int a[1] = {0};
    return std::accumulate(a, a + 1, INT_MAX) == INT_MAX;
}
static_assert(prism_acc_hi());

constexpr bool prism_acc_imin() {
    int a[1] = {INT_MIN};
    return std::accumulate(a, a + 1, 0) == INT_MIN;
}
static_assert(prism_acc_imin());

constexpr bool prism_adj_seq() {
    int a[4] = {3, 5, 9, 12};
    int d[4] = {};
    int* end = std::adjacent_difference(a, a + 4, d);
    return end == d + 4 && d[0] == 3 && d[1] == 2 && d[2] == 4 && d[3] == 3;
}
static_assert(prism_adj_seq());

constexpr bool prism_adj_empty() {
    int a[1] = {4};
    int d[1] = {7};
    int* end = std::adjacent_difference(a, a, d);
    return end == d && d[0] == 7 && a[0] == 4;
}
static_assert(prism_adj_empty());

constexpr bool prism_adj_neg() {
    int a[3] = {-1, 0, 1};
    int d[3] = {};
    std::adjacent_difference(a, a + 3, d);
    return d[0] == -1 && d[1] == 1 && d[2] == 1;
}
static_assert(prism_adj_neg());

constexpr bool prism_adj_min() {
    int a[3] = {INT_MIN, INT_MIN + 1, INT_MIN + 2};
    int d[3] = {};
    std::adjacent_difference(a, a + 3, d);
    return d[0] == INT_MIN && d[1] == 1 && d[2] == 1;
}
static_assert(prism_adj_min());

constexpr bool prism_adj_same() {
    int a[2] = {INT_MAX, INT_MAX};
    int d[2] = {};
    std::adjacent_difference(a, a + 2, d);
    return d[0] == INT_MAX && d[1] == 0;
}
static_assert(prism_adj_same());

constexpr bool prism_adj_minsame() {
    int a[2] = {INT_MIN, INT_MIN};
    int d[2] = {};
    std::adjacent_difference(a, a + 2, d);
    return d[0] == INT_MIN && d[1] == 0;
}
static_assert(prism_adj_minsame());

constexpr bool prism_adj_sc() {
    signed char a[2] = {static_cast<signed char>(SCHAR_MAX - 1), static_cast<signed char>(SCHAR_MAX)};
    signed char d[2] = {};
    std::adjacent_difference(a, a + 2, d);
    return d[0] == static_cast<signed char>(SCHAR_MAX - 1) && d[1] == static_cast<signed char>(1);
}
static_assert(prism_adj_sc());

constexpr bool prism_adj_uc() {
    unsigned char a[2] = {static_cast<unsigned char>(UCHAR_MAX - 1), static_cast<unsigned char>(UCHAR_MAX)};
    unsigned char d[2] = {};
    std::adjacent_difference(a, a + 2, d);
    return d[0] == static_cast<unsigned char>(UCHAR_MAX - 1) && d[1] == static_cast<unsigned char>(1);
}
static_assert(prism_adj_uc());

constexpr bool prism_adj_f() {
    float a[3] = {0.0f, 1.0f, 3.0f};
    float d[3] = {};
    std::adjacent_difference(a, a + 3, d);
    return d[0] == 0.0f && d[1] == 1.0f && d[2] == 2.0f;
}
static_assert(prism_adj_f());

constexpr bool prism_adj_b() {
    bool a[3] = {false, true, true};
    bool d[3] = {true, true, true};
    std::adjacent_difference(a, a + 3, d);
    return d[0] == false && d[1] == true && d[2] == false;
}
static_assert(prism_adj_b());

constexpr bool prism_ps_seq() {
    int a[4] = {1, 2, 3, 4};
    int d[4] = {};
    int* end = std::partial_sum(a, a + 4, d);
    return end == d + 4 && a[0] == 1 && a[3] == 4 && d[0] == 1 && d[1] == 3 && d[2] == 6 && d[3] == 10;
}
static_assert(prism_ps_seq());

constexpr bool prism_ps_empty() {
    int a[1] = {7};
    int d[1] = {9};
    int* end = std::partial_sum(a, a, d);
    return end == d && d[0] == 9 && a[0] == 7;
}
static_assert(prism_ps_empty());

constexpr bool prism_ps_empty_null() {
    int a[1] = {4};
    int* end = std::partial_sum(a, a, static_cast<int*>(nullptr));
    return end == nullptr && a[0] == 4;
}
static_assert(prism_ps_empty_null());

constexpr bool prism_ps_neg() {
    int a[3] = {-5, 3, 4};
    int d[3] = {};
    std::partial_sum(a, a + 3, d);
    return d[0] == -5 && d[1] == -2 && d[2] == 2;
}
static_assert(prism_ps_neg());

constexpr bool prism_ps_imin() {
    int a[3] = {0, INT_MIN, 1};
    int d[3] = {};
    std::partial_sum(a, a + 3, d);
    return d[0] == 0 && d[1] == INT_MIN && d[2] == INT_MIN + 1;
}
static_assert(prism_ps_imin());

constexpr bool prism_ps_hi() {
    int a[1] = {INT_MAX};
    int d[1] = {0};
    int* end = std::partial_sum(a, a + 1, d);
    return end == d + 1 && d[0] == INT_MAX;
}
static_assert(prism_ps_hi());

constexpr bool prism_ps_zeroadd() {
    int a[2] = {INT_MAX, 0};
    int d[2] = {};
    std::partial_sum(a, a + 2, d);
    return d[0] == INT_MAX && d[1] == INT_MAX;
}
static_assert(prism_ps_zeroadd());

constexpr bool prism_ps_min0() {
    int a[2] = {INT_MIN, 0};
    int d[2] = {};
    std::partial_sum(a, a + 2, d);
    return d[0] == INT_MIN && d[1] == INT_MIN;
}
static_assert(prism_ps_min0());

constexpr bool prism_ps_sc() {
    signed char a[3] = {static_cast<signed char>(SCHAR_MAX - 2), 1, 1};
    signed char d[3] = {};
    std::partial_sum(a, a + 3, d);
    return d[0] == static_cast<signed char>(SCHAR_MAX - 2) && d[1] == static_cast<signed char>(SCHAR_MAX - 1) &&
           d[2] == static_cast<signed char>(SCHAR_MAX);
}
static_assert(prism_ps_sc());

constexpr bool prism_ps_uc() {
    unsigned char a[3] = {static_cast<unsigned char>(UCHAR_MAX - 5), 2, 3};
    unsigned char d[3] = {};
    std::partial_sum(a, a + 3, d);
    return d[0] == static_cast<unsigned char>(UCHAR_MAX - 5) &&
           d[1] == static_cast<unsigned char>(UCHAR_MAX - 3) && d[2] == static_cast<unsigned char>(UCHAR_MAX);
}
static_assert(prism_ps_uc());

constexpr bool prism_ps_f() {
    float a[3] = {2.0f, 3.0f, 4.0f};
    float d[3] = {};
    std::partial_sum(a, a + 3, d);
    return d[0] == 2.0f && d[1] == 5.0f && d[2] == 9.0f;
}
static_assert(prism_ps_f());

constexpr bool prism_ps_b() {
    bool a[3] = {false, true, true};
    bool d[3] = {true, true, true};
    std::partial_sum(a, a + 3, d);
    return d[0] == false && d[1] == true && d[2] == true;
}
static_assert(prism_ps_b());

constexpr bool prism_ps_bf() {
    bool a[3] = {false, false, true};
    bool d[3] = {true, true, true};
    std::partial_sum(a, a + 3, d);
    return d[0] == false && d[1] == false && d[2] == true;
}
static_assert(prism_ps_bf());

constexpr bool prism_ps_tail() {
    int a[4] = {1, 2, 8, 8};
    int* end = std::partial_sum(a, a + 2, a + 2);
    return end == a + 4 && a[0] == 1 && a[1] == 2 && a[2] == 1 && a[3] == 3;
}
static_assert(prism_ps_tail());

constexpr bool prism_ps_u() {
    unsigned a[3] = {UINT_MAX - 2u, 1u, 1u};
    unsigned d[3] = {};
    std::partial_sum(a, a + 3, d);
    return d[0] == UINT_MAX - 2u && d[1] == UINT_MAX - 1u && d[2] == UINT_MAX;
}
static_assert(prism_ps_u());

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
    if (arg_is(argc, argv, "iota-ovf")) {
        int a[2] = {4, 5};
        volatile int s = INT_MAX;
        std::iota(a, a + 2, s);
        std::printf("wrapped=%d,%d\n", a[0], a[1]);
        return 0;
    }
    if (arg_is(argc, argv, "iota-llovf")) {
        long long a[2] = {4, 5};
        volatile long long s = LLONG_MAX;
        std::iota(a, a + 2, s);
        std::printf("wrapped=%lld,%lld\n", a[0], a[1]);
        return 0;
    }
    if (arg_is(argc, argv, "iota-uovf")) {
        unsigned a[2] = {4u, 5u};
        volatile unsigned s = UINT_MAX;
        std::iota(a, a + 2, s);
        std::printf("wrapped=%u,%u\n", a[0], a[1]);
        return 0;
    }
    if (arg_is(argc, argv, "iota-inv")) {
        int a[2] = {4, 5};
        std::iota(a + 1, a, 0);
        std::printf("wrapped=%d,%d\n", a[0], a[1]);
        return 0;
    }
    if (arg_is(argc, argv, "iota-max1")) {
        int a[1] = {0};
        volatile int s = INT_MAX;
        std::iota(a, a + 1, s);
        std::printf("iota_hi=%d\n", a[0]);
        return 0;
    }
    if (arg_is(argc, argv, "acc-ovf")) {
        int a[1] = {1};
        volatile int s = INT_MAX;
        const int r = std::accumulate(a, a + 1, s);
        std::printf("wrapped=%d\n", r);
        return 0;
    }
    if (arg_is(argc, argv, "acc-llovf")) {
        long long a[1] = {1};
        volatile long long s = LLONG_MAX;
        const long long r = std::accumulate(a, a + 1, s);
        std::printf("wrapped=%lld\n", r);
        return 0;
    }
    if (arg_is(argc, argv, "acc-uovf")) {
        unsigned a[1] = {1u};
        volatile unsigned s = UINT_MAX;
        const unsigned r = std::accumulate(a, a + 1, s);
        std::printf("wrapped=%u\n", r);
        return 0;
    }
    if (arg_is(argc, argv, "acc-minovf")) {
        int a[1] = {-1};
        volatile int s = INT_MIN;
        const int r = std::accumulate(a, a + 1, s);
        std::printf("wrapped=%d\n", r);
        return 0;
    }
    if (arg_is(argc, argv, "acc-sc")) {
        signed char a[1] = {1};
        volatile signed char s = static_cast<signed char>(SCHAR_MAX);
        const signed char r = std::accumulate(a, a + 1, s);
        std::printf("wrapped=%d\n", static_cast<int>(r));
        return 0;
    }
    if (arg_is(argc, argv, "acc-part")) {
        int a[2] = {INT_MAX, 1};
        const int r = std::accumulate(a, a + 2, 0);
        std::printf("wrapped=%d\n", r);
        return 0;
    }
    if (arg_is(argc, argv, "acc-inv")) {
        int a[2] = {4, 5};
        const int r = std::accumulate(a + 1, a, 0);
        std::printf("wrapped=%d\n", r);
        return 0;
    }
    if (arg_is(argc, argv, "acc-max1")) {
        int a[1] = {0};
        volatile int s = INT_MAX;
        const int r = std::accumulate(a, a + 1, s);
        std::printf("acc_hi=%d\n", r);
        return 0;
    }
    if (arg_is(argc, argv, "adj-ovf")) {
        int a[2] = {-1, INT_MAX};
        int d[2] = {4, 5};
        std::adjacent_difference(a, a + 2, d);
        std::printf("wrapped=%d,%d\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "adj-llovf")) {
        long long a[2] = {-1, LLONG_MAX};
        long long d[2] = {4, 5};
        std::adjacent_difference(a, a + 2, d);
        std::printf("wrapped=%lld,%lld\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "adj-minovf")) {
        int a[2] = {1, INT_MIN};
        int d[2] = {4, 5};
        std::adjacent_difference(a, a + 2, d);
        std::printf("wrapped=%d,%d\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "adj-sc")) {
        signed char a[2] = {-1, static_cast<signed char>(SCHAR_MAX)};
        signed char d[2] = {4, 5};
        std::adjacent_difference(a, a + 2, d);
        std::printf("wrapped=%d,%d\n", static_cast<int>(d[0]), static_cast<int>(d[1]));
        return 0;
    }
    if (arg_is(argc, argv, "adj-part")) {
        int a[3] = {5, 8, INT_MIN};
        int d[3] = {4, 4, 4};
        std::adjacent_difference(a, a + 3, d);
        std::printf("wrapped=%d,%d,%d\n", d[0], d[1], d[2]);
        return 0;
    }
    if (arg_is(argc, argv, "adj-uovf")) {
        unsigned a[2] = {1u, 0u};
        unsigned d[2] = {4u, 5u};
        std::adjacent_difference(a, a + 2, d);
        std::printf("wrapped=%u,%u\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "adj-inv")) {
        int a[2] = {4, 5};
        int d[2] = {7, 8};
        std::adjacent_difference(a + 1, a, d);
        std::printf("wrapped=%d,%d\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "adj-null")) {
        int a[2] = {4, 5};
        std::adjacent_difference(a, a + 2, static_cast<int*>(nullptr));
        std::printf("wrapped=%d,%d\n", a[0], a[1]);
        return 0;
    }
    if (arg_is(argc, argv, "adj-over")) {
        int a[3] = {3, 5, 9};
        std::adjacent_difference(a, a + 3, a + 1);
        std::printf("wrapped=%d,%d,%d\n", a[0], a[1], a[2]);
        return 0;
    }
    if (arg_is(argc, argv, "adj-max1")) {
        int a[1] = {INT_MAX};
        int d[1] = {0};
        std::adjacent_difference(a, a + 1, d);
        std::printf("adj_hi=%d\n", d[0]);
        return 0;
    }
    if (arg_is(argc, argv, "ps-ovf")) {
        int a[2] = {INT_MAX, 1};
        int d[2] = {4, 5};
        std::partial_sum(a, a + 2, d);
        std::printf("wrapped=%d,%d\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "ps-llovf")) {
        long long a[2] = {LLONG_MAX, 1};
        long long d[2] = {4, 5};
        std::partial_sum(a, a + 2, d);
        std::printf("wrapped=%lld,%lld\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "ps-minovf")) {
        int a[2] = {INT_MIN, -1};
        int d[2] = {4, 5};
        std::partial_sum(a, a + 2, d);
        std::printf("wrapped=%d,%d\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "ps-sc")) {
        signed char a[2] = {static_cast<signed char>(SCHAR_MAX), 1};
        signed char d[2] = {4, 5};
        std::partial_sum(a, a + 2, d);
        std::printf("wrapped=%d,%d\n", static_cast<int>(d[0]), static_cast<int>(d[1]));
        return 0;
    }
    if (arg_is(argc, argv, "ps-part")) {
        int a[3] = {5, 8, INT_MAX};
        int d[3] = {4, 4, 4};
        std::partial_sum(a, a + 3, d);
        std::printf("wrapped=%d,%d,%d\n", d[0], d[1], d[2]);
        return 0;
    }
    if (arg_is(argc, argv, "ps-uovf")) {
        unsigned a[2] = {1u, UINT_MAX};
        unsigned d[2] = {4u, 5u};
        std::partial_sum(a, a + 2, d);
        std::printf("wrapped=%u,%u\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "ps-ucovf")) {
        unsigned char a[2] = {static_cast<unsigned char>(UCHAR_MAX), 1};
        unsigned char d[2] = {4, 5};
        std::partial_sum(a, a + 2, d);
        std::printf("wrapped=%u,%u\n", static_cast<unsigned>(d[0]), static_cast<unsigned>(d[1]));
        return 0;
    }
    if (arg_is(argc, argv, "ps-inv")) {
        int a[2] = {4, 5};
        int d[2] = {7, 8};
        std::partial_sum(a + 1, a, d);
        std::printf("wrapped=%d,%d\n", d[0], d[1]);
        return 0;
    }
    if (arg_is(argc, argv, "ps-null")) {
        int a[2] = {4, 5};
        std::partial_sum(a, a + 2, static_cast<int*>(nullptr));
        std::printf("wrapped=%d,%d\n", a[0], a[1]);
        return 0;
    }
    if (arg_is(argc, argv, "ps-over")) {
        int a[3] = {3, 5, 9};
        std::partial_sum(a, a + 3, a + 1);
        std::printf("wrapped=%d,%d,%d\n", a[0], a[1], a[2]);
        return 0;
    }
    if (arg_is(argc, argv, "ps-max1")) {
        int a[1] = {INT_MAX};
        int d[1] = {0};
        int* end = std::partial_sum(a, a + 1, d);
        std::printf("ps_hi=%d end=%d\n", d[0], static_cast<int>(end - d));
        return 0;
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
    int iota_seq[4] = {9, 9, 9, 9};
    std::iota(iota_seq, iota_seq + 4, 0);
    std::printf("iota=%d,%d,%d,%d\n", iota_seq[0], iota_seq[1], iota_seq[2], iota_seq[3]);
    int iota_empty[1] = {7};
    std::iota(iota_empty, iota_empty, 1);
    std::printf("iota_empty=%d\n", iota_empty[0]);
    int iota_neg[3] = {};
    std::iota(iota_neg, iota_neg + 3, -1);
    std::printf("iota_neg=%d,%d,%d\n", iota_neg[0], iota_neg[1], iota_neg[2]);
    int iota_min[3] = {};
    std::iota(iota_min, iota_min + 3, INT_MIN);
    std::printf("iota_min=%d,%d,%d\n", iota_min[0], iota_min[1], iota_min[2]);
    int iota_near[2] = {};
    std::iota(iota_near, iota_near + 2, INT_MAX - 2);
    std::printf("iota_near=%d,%d\n", iota_near[0], iota_near[1]);
    unsigned iota_u[3] = {};
    std::iota(iota_u, iota_u + 3, 1u);
    std::printf("iota_u=%u,%u,%u\n", iota_u[0], iota_u[1], iota_u[2]);
    unsigned iota_unear[2] = {};
    std::iota(iota_unear, iota_unear + 2, UINT_MAX - 2u);
    std::printf("iota_unear=%u,%u\n", iota_unear[0], iota_unear[1]);
    unsigned iota_umax[1] = {0};
    std::iota(iota_umax, iota_umax + 1, UINT_MAX);
    std::printf("iota_umax=%u\n", iota_umax[0]);
    int iota_one[1] = {0};
    std::iota(iota_one, iota_one + 1, 41);
    std::printf("iota_one=%d\n", iota_one[0]);
    signed char iota_sc[2] = {};
    std::iota(iota_sc, iota_sc + 2, static_cast<signed char>(SCHAR_MAX - 2));
    std::printf("iota_sc=%d,%d\n", static_cast<int>(iota_sc[0]), static_cast<int>(iota_sc[1]));
    unsigned char iota_uc[2] = {};
    std::iota(iota_uc, iota_uc + 2, static_cast<unsigned char>(UCHAR_MAX - 1));
    std::printf("iota_uc=%u,%u\n", static_cast<unsigned>(iota_uc[0]), static_cast<unsigned>(iota_uc[1]));
    float iota_f[3] = {};
    std::iota(iota_f, iota_f + 3, 0.0f);
    std::printf("iota_f=%d,%d,%d\n", static_cast<int>(iota_f[0]), static_cast<int>(iota_f[1]),
                static_cast<int>(iota_f[2]));
    int acc_seq[4] = {1, 2, 3, 4};
    std::printf("acc=%d\n", std::accumulate(acc_seq, acc_seq + 4, 0));
    int acc_empty[1] = {7};
    std::printf("acc_empty=%d,%d\n", std::accumulate(acc_empty, acc_empty, 3), acc_empty[0]);
    int acc_neg[3] = {-1, 0, 1};
    std::printf("acc_neg=%d\n", std::accumulate(acc_neg, acc_neg + 3, 0));
    int acc_min[3] = {INT_MIN, 1, 1};
    std::printf("acc_min=%d\n", std::accumulate(acc_min, acc_min + 3, 0));
    int acc_near[2] = {1, 1};
    std::printf("acc_near=%d\n", std::accumulate(acc_near, acc_near + 2, INT_MAX - 2));
    int acc_hi_slot[1] = {0};
    std::printf("acc_hi=%d\n", std::accumulate(acc_hi_slot, acc_hi_slot + 1, INT_MAX));
    int acc_imin[1] = {INT_MIN};
    std::printf("acc_imin=%d\n", std::accumulate(acc_imin, acc_imin + 1, 0));
    unsigned acc_u[3] = {1u, 2u, 3u};
    std::printf("acc_u=%u\n", std::accumulate(acc_u, acc_u + 3, 0u));
    unsigned acc_unear[2] = {1u, 1u};
    std::printf("acc_unear=%u\n", std::accumulate(acc_unear, acc_unear + 2, UINT_MAX - 2u));
    unsigned acc_umax[1] = {UINT_MAX};
    std::printf("acc_umax=%u\n", std::accumulate(acc_umax, acc_umax + 1, 0u));
    unsigned acc_uzero[1] = {0u};
    std::printf("acc_uzero=%u\n", std::accumulate(acc_uzero, acc_uzero + 1, UINT_MAX));
    int acc_one[1] = {1};
    std::printf("acc_one=%d\n", std::accumulate(acc_one, acc_one + 1, 40));
    signed char acc_sc[2] = {1, 1};
    std::printf("acc_sc=%d\n", static_cast<int>(std::accumulate(
                                   acc_sc, acc_sc + 2, static_cast<signed char>(SCHAR_MAX - 2))));
    unsigned char acc_uc[2] = {1, 0};
    std::printf("acc_uc=%u\n", static_cast<unsigned>(std::accumulate(
                                   acc_uc, acc_uc + 2, static_cast<unsigned char>(UCHAR_MAX - 1))));
    float acc_f[3] = {0.0f, 1.0f, 1.0f};
    std::printf("acc_f=%d\n", static_cast<int>(std::accumulate(acc_f, acc_f + 3, 0.0f)));
    bool acc_b[2] = {true, true};
    std::printf("acc_b=%d\n", std::accumulate(acc_b, acc_b + 2, false) ? 1 : 0);
    int adj_src[4] = {3, 5, 9, 12};
    int adj_dst[4] = {};
    int* adj_end = std::adjacent_difference(adj_src, adj_src + 4, adj_dst);
    std::printf("adj=%d,%d,%d,%d end=%d\n", adj_dst[0], adj_dst[1], adj_dst[2], adj_dst[3],
                static_cast<int>(adj_end - adj_dst));
    int adj_empty_src[1] = {4};
    int adj_empty[1] = {7};
    int* adj_empty_end = std::adjacent_difference(adj_empty_src, adj_empty_src, adj_empty);
    std::printf("adj_empty=%d end=%d\n", adj_empty[0], adj_empty_end == adj_empty ? 1 : 0);
    int adj_neg_src[3] = {-1, 0, 1};
    int adj_neg[3] = {};
    std::adjacent_difference(adj_neg_src, adj_neg_src + 3, adj_neg);
    std::printf("adj_neg=%d,%d,%d\n", adj_neg[0], adj_neg[1], adj_neg[2]);
    int adj_min_src[3] = {INT_MIN, INT_MIN + 1, INT_MIN + 2};
    int adj_min[3] = {};
    std::adjacent_difference(adj_min_src, adj_min_src + 3, adj_min);
    std::printf("adj_min=%d,%d,%d\n", adj_min[0], adj_min[1], adj_min[2]);
    int adj_near_src[3] = {INT_MAX - 2, INT_MAX - 1, INT_MAX};
    int adj_near[3] = {};
    std::adjacent_difference(adj_near_src, adj_near_src + 3, adj_near);
    std::printf("adj_near=%d,%d,%d\n", adj_near[0], adj_near[1], adj_near[2]);
    int adj_imax_src[2] = {0, INT_MAX};
    int adj_imax[2] = {};
    std::adjacent_difference(adj_imax_src, adj_imax_src + 2, adj_imax);
    std::printf("adj_imax=%d,%d\n", adj_imax[0], adj_imax[1]);
    int adj_imin_src[2] = {0, INT_MIN};
    int adj_imin[2] = {};
    std::adjacent_difference(adj_imin_src, adj_imin_src + 2, adj_imin);
    std::printf("adj_imin=%d,%d\n", adj_imin[0], adj_imin[1]);
    int adj_same_src[2] = {INT_MAX, INT_MAX};
    int adj_same[2] = {};
    std::adjacent_difference(adj_same_src, adj_same_src + 2, adj_same);
    std::printf("adj_same=%d,%d\n", adj_same[0], adj_same[1]);
    int adj_minsame_src[2] = {INT_MIN, INT_MIN};
    int adj_minsame[2] = {};
    std::adjacent_difference(adj_minsame_src, adj_minsame_src + 2, adj_minsame);
    std::printf("adj_minsame=%d,%d\n", adj_minsame[0], adj_minsame[1]);
    int adj_eq_src[3] = {4, 4, 4};
    int adj_eq[3] = {};
    std::adjacent_difference(adj_eq_src, adj_eq_src + 3, adj_eq);
    std::printf("adj_eq=%d,%d,%d\n", adj_eq[0], adj_eq[1], adj_eq[2]);
    unsigned adj_u_src[3] = {1u, 2u, 4u};
    unsigned adj_u[3] = {};
    std::adjacent_difference(adj_u_src, adj_u_src + 3, adj_u);
    std::printf("adj_u=%u,%u,%u\n", adj_u[0], adj_u[1], adj_u[2]);
    unsigned adj_unear_src[3] = {UINT_MAX - 2u, UINT_MAX - 1u, UINT_MAX};
    unsigned adj_unear[3] = {};
    std::adjacent_difference(adj_unear_src, adj_unear_src + 3, adj_unear);
    std::printf("adj_unear=%u,%u,%u\n", adj_unear[0], adj_unear[1], adj_unear[2]);
    unsigned adj_umax_src[1] = {UINT_MAX};
    unsigned adj_umax[1] = {0u};
    std::adjacent_difference(adj_umax_src, adj_umax_src + 1, adj_umax);
    std::printf("adj_umax=%u\n", adj_umax[0]);
    unsigned adj_usame_src[2] = {UINT_MAX, UINT_MAX};
    unsigned adj_usame[2] = {};
    std::adjacent_difference(adj_usame_src, adj_usame_src + 2, adj_usame);
    std::printf("adj_usame=%u,%u\n", adj_usame[0], adj_usame[1]);
    int adj_one_src[1] = {41};
    int adj_one[1] = {0};
    std::adjacent_difference(adj_one_src, adj_one_src + 1, adj_one);
    std::printf("adj_one=%d\n", adj_one[0]);
    int adj_hi_src[1] = {INT_MAX};
    int adj_hi[1] = {0};
    std::adjacent_difference(adj_hi_src, adj_hi_src + 1, adj_hi);
    std::printf("adj_hi=%d\n", adj_hi[0]);
    signed char adj_sc_src[2] = {static_cast<signed char>(SCHAR_MAX - 1), static_cast<signed char>(SCHAR_MAX)};
    signed char adj_sc[2] = {};
    std::adjacent_difference(adj_sc_src, adj_sc_src + 2, adj_sc);
    std::printf("adj_sc=%d,%d\n", static_cast<int>(adj_sc[0]), static_cast<int>(adj_sc[1]));
    signed char adj_scn_src[2] = {2, -1};
    signed char adj_scn[2] = {};
    std::adjacent_difference(adj_scn_src, adj_scn_src + 2, adj_scn);
    std::printf("adj_scn=%d,%d\n", static_cast<int>(adj_scn[0]), static_cast<int>(adj_scn[1]));
    unsigned char adj_uc_src[2] = {static_cast<unsigned char>(UCHAR_MAX - 1), static_cast<unsigned char>(UCHAR_MAX)};
    unsigned char adj_uc[2] = {};
    std::adjacent_difference(adj_uc_src, adj_uc_src + 2, adj_uc);
    std::printf("adj_uc=%u,%u\n", static_cast<unsigned>(adj_uc[0]), static_cast<unsigned>(adj_uc[1]));
    float adj_f_src[3] = {0.0f, 1.0f, 3.0f};
    float adj_f[3] = {};
    std::adjacent_difference(adj_f_src, adj_f_src + 3, adj_f);
    std::printf("adj_f=%d,%d,%d\n", static_cast<int>(adj_f[0]), static_cast<int>(adj_f[1]),
                static_cast<int>(adj_f[2]));
    bool adj_b_src[3] = {false, true, true};
    bool adj_b[3] = {true, true, true};
    std::adjacent_difference(adj_b_src, adj_b_src + 3, adj_b);
    std::printf("adj_b=%d,%d,%d\n", adj_b[0] ? 1 : 0, adj_b[1] ? 1 : 0, adj_b[2] ? 1 : 0);
    int adj_tail[4] = {8, 3, 9, 9};
    int* adj_tail_end = std::adjacent_difference(adj_tail, adj_tail + 2, adj_tail + 2);
    std::printf("adj_tail=%d,%d,%d,%d end=%d\n", adj_tail[0], adj_tail[1], adj_tail[2], adj_tail[3],
                static_cast<int>(adj_tail_end - (adj_tail + 2)));
    int adj_en_src[1] = {4};
    int* adj_en = std::adjacent_difference(adj_en_src, adj_en_src, static_cast<int*>(nullptr));
    std::printf("adj_empty_null=%d\n", adj_en == nullptr ? 1 : 0);
    long long adj_ll_src[2] = {1, 3};
    long long adj_ll[2] = {};
    std::adjacent_difference(adj_ll_src, adj_ll_src + 2, adj_ll);
    std::printf("adj_ll=%lld,%lld\n", adj_ll[0], adj_ll[1]);
    int ps_src[4] = {1, 2, 3, 4};
    int ps_dst[4] = {};
    int* ps_end = std::partial_sum(ps_src, ps_src + 4, ps_dst);
    std::printf("ps=%d,%d,%d,%d end=%d\n", ps_dst[0], ps_dst[1], ps_dst[2], ps_dst[3],
                static_cast<int>(ps_end - ps_dst));
    int ps_empty_src[1] = {7};
    int ps_empty[1] = {9};
    int* ps_empty_end = std::partial_sum(ps_empty_src, ps_empty_src, ps_empty);
    std::printf("ps_empty=%d,%d end=%d\n", ps_empty[0], ps_empty_src[0], ps_empty_end == ps_empty ? 1 : 0);
    int ps_neg_src[3] = {-5, 3, 4};
    int ps_neg[3] = {};
    std::partial_sum(ps_neg_src, ps_neg_src + 3, ps_neg);
    std::printf("ps_neg=%d,%d,%d\n", ps_neg[0], ps_neg[1], ps_neg[2]);
    int ps_imin_src[3] = {0, INT_MIN, 1};
    int ps_imin[3] = {};
    std::partial_sum(ps_imin_src, ps_imin_src + 3, ps_imin);
    std::printf("ps_imin=%d,%d,%d\n", ps_imin[0], ps_imin[1], ps_imin[2]);
    int ps_near_src[3] = {INT_MAX - 2, 1, 1};
    int ps_near[3] = {};
    std::partial_sum(ps_near_src, ps_near_src + 3, ps_near);
    std::printf("ps_near=%d,%d,%d\n", ps_near[0], ps_near[1], ps_near[2]);
    int ps_down_src[3] = {INT_MAX, -5, 3};
    int ps_down[3] = {};
    std::partial_sum(ps_down_src, ps_down_src + 3, ps_down);
    std::printf("ps_down=%d,%d,%d\n", ps_down[0], ps_down[1], ps_down[2]);
    int ps_zero_src[2] = {INT_MAX, 0};
    int ps_zero[2] = {};
    std::partial_sum(ps_zero_src, ps_zero_src + 2, ps_zero);
    std::printf("ps_zeroadd=%d,%d\n", ps_zero[0], ps_zero[1]);
    int ps_min0_src[2] = {INT_MIN, 0};
    int ps_min0[2] = {};
    std::partial_sum(ps_min0_src, ps_min0_src + 2, ps_min0);
    std::printf("ps_min0=%d,%d\n", ps_min0[0], ps_min0[1]);
    int ps_eq_src[3] = {4, 4, 4};
    int ps_eq[3] = {};
    std::partial_sum(ps_eq_src, ps_eq_src + 3, ps_eq);
    std::printf("ps_eq=%d,%d,%d\n", ps_eq[0], ps_eq[1], ps_eq[2]);
    unsigned ps_u_src[3] = {1u, 2u, 3u};
    unsigned ps_u[3] = {};
    std::partial_sum(ps_u_src, ps_u_src + 3, ps_u);
    std::printf("ps_u=%u,%u,%u\n", ps_u[0], ps_u[1], ps_u[2]);
    unsigned ps_unear_src[3] = {UINT_MAX - 2u, 1u, 1u};
    unsigned ps_unear[3] = {};
    std::partial_sum(ps_unear_src, ps_unear_src + 3, ps_unear);
    std::printf("ps_unear=%u,%u,%u\n", ps_unear[0], ps_unear[1], ps_unear[2]);
    unsigned ps_umax_src[1] = {UINT_MAX};
    unsigned ps_umax[1] = {0u};
    std::partial_sum(ps_umax_src, ps_umax_src + 1, ps_umax);
    std::printf("ps_umax=%u\n", ps_umax[0]);
    unsigned ps_uzero_src[2] = {UINT_MAX, 0u};
    unsigned ps_uzero[2] = {};
    std::partial_sum(ps_uzero_src, ps_uzero_src + 2, ps_uzero);
    std::printf("ps_uzero=%u,%u\n", ps_uzero[0], ps_uzero[1]);
    int ps_one_src[1] = {41};
    int ps_one[1] = {0};
    int* ps_one_end = std::partial_sum(ps_one_src, ps_one_src + 1, ps_one);
    std::printf("ps_one=%d end=%d\n", ps_one[0], static_cast<int>(ps_one_end - ps_one));
    int ps_hi_src[1] = {INT_MAX};
    int ps_hi[1] = {0};
    int* ps_hi_end = std::partial_sum(ps_hi_src, ps_hi_src + 1, ps_hi);
    std::printf("ps_hi=%d end=%d\n", ps_hi[0], static_cast<int>(ps_hi_end - ps_hi));
    signed char ps_sc_src[3] = {static_cast<signed char>(SCHAR_MAX - 2), 1, 1};
    signed char ps_sc[3] = {};
    std::partial_sum(ps_sc_src, ps_sc_src + 3, ps_sc);
    std::printf("ps_sc=%d,%d,%d\n", static_cast<int>(ps_sc[0]), static_cast<int>(ps_sc[1]), static_cast<int>(ps_sc[2]));
    signed char ps_scn_src[2] = {-2, -3};
    signed char ps_scn[2] = {};
    std::partial_sum(ps_scn_src, ps_scn_src + 2, ps_scn);
    std::printf("ps_scn=%d,%d\n", static_cast<int>(ps_scn[0]), static_cast<int>(ps_scn[1]));
    unsigned char ps_uc_src[3] = {static_cast<unsigned char>(UCHAR_MAX - 5), 2, 3};
    unsigned char ps_uc[3] = {};
    std::partial_sum(ps_uc_src, ps_uc_src + 3, ps_uc);
    std::printf("ps_uc=%u,%u,%u\n", static_cast<unsigned>(ps_uc[0]), static_cast<unsigned>(ps_uc[1]),
                static_cast<unsigned>(ps_uc[2]));
    float ps_f_src[3] = {2.0f, 3.0f, 4.0f};
    float ps_f[3] = {};
    std::partial_sum(ps_f_src, ps_f_src + 3, ps_f);
    std::printf("ps_f=%d,%d,%d\n", static_cast<int>(ps_f[0]), static_cast<int>(ps_f[1]), static_cast<int>(ps_f[2]));
    bool ps_b_src[3] = {false, true, true};
    bool ps_b[3] = {true, true, true};
    std::partial_sum(ps_b_src, ps_b_src + 3, ps_b);
    std::printf("ps_b=%d,%d,%d\n", ps_b[0] ? 1 : 0, ps_b[1] ? 1 : 0, ps_b[2] ? 1 : 0);
    bool ps_bf_src[3] = {false, false, true};
    bool ps_bf[3] = {true, true, true};
    std::partial_sum(ps_bf_src, ps_bf_src + 3, ps_bf);
    std::printf("ps_bf=%d,%d,%d\n", ps_bf[0] ? 1 : 0, ps_bf[1] ? 1 : 0, ps_bf[2] ? 1 : 0);
    int ps_tail[4] = {1, 2, 8, 8};
    int* ps_tail_end = std::partial_sum(ps_tail, ps_tail + 2, ps_tail + 2);
    std::printf("ps_tail=%d,%d,%d,%d end=%d\n", ps_tail[0], ps_tail[1], ps_tail[2], ps_tail[3],
                static_cast<int>(ps_tail_end - (ps_tail + 2)));
    int ps_en_src[1] = {4};
    int* ps_en = std::partial_sum(ps_en_src, ps_en_src, static_cast<int*>(nullptr));
    std::printf("ps_empty_null=%d\n", ps_en == nullptr ? 1 : 0);
    long long ps_ll_src[3] = {1, 2, 3};
    long long ps_ll[3] = {};
    std::partial_sum(ps_ll_src, ps_ll_src + 3, ps_ll);
    std::printf("ps_ll=%lld,%lld,%lld\n", ps_ll[0], ps_ll[1], ps_ll[2]);
    return 0;
}
