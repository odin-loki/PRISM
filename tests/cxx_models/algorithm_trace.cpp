// Differential trace of <algorithm> (tests/cpp/test_cxx_algorithm.cpp).
// Compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/algorithm) first on the include path.
// min and max return one of the two arguments. Equal arguments return the
// first. clamp of a valid range returns lo, hi, or v. min of the two
// extreme ints is the lower one, not 0. sort, find, copy, fill, and
// for_each are not called.
// argv "invert" calls clamp with hi < lo. The model must __prism_check
// and must not print a value.
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned value. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

struct Ord {
    int x;
    friend constexpr bool operator<(const Ord& p, const Ord& q) { return p.x < q.x; }
};

static int same_int(const int& r, const int& x) { return &r == &x ? 1 : 0; }

static_assert(std::min(3, 7) == 3);
static_assert(std::min(7, 3) == 3);
static_assert(std::min(4, 4) == 4);
static_assert(std::max(3, 7) == 7);
static_assert(std::max(7, 3) == 7);
static_assert(std::max(4, 4) == 4);
static_assert(std::clamp(5, 3, 7) == 5);
static_assert(std::clamp(1, 3, 7) == 3);
static_assert(std::clamp(9, 3, 7) == 7);
static_assert(std::clamp(3, 3, 7) == 3);
static_assert(std::clamp(7, 3, 7) == 7);
static_assert(std::min(false, true) == false);
static_assert(std::max(false, true) == true);
static_assert(std::min(INT_MIN, INT_MAX) == INT_MIN);
static_assert(std::max(INT_MIN, INT_MAX) == INT_MAX);
static_assert(std::min(INT_MIN, INT_MAX) != 0);
static_assert(std::max(INT_MIN, INT_MAX) != 0);
static_assert(std::clamp(0, INT_MIN, INT_MAX) == 0);
static_assert(std::clamp(INT_MIN, INT_MIN, INT_MAX) == INT_MIN);
static_assert(std::clamp(INT_MAX, INT_MIN, INT_MAX) == INT_MAX);

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "invert") == 0) {
        volatile int vlo = 4;
        volatile int vhi = 1;
        volatile int vv = 0;
        int lo = vlo;
        int hi = vhi;
        int v = vv;
        int r = std::clamp(v, lo, hi);
        std::printf("value=%d\n", r);
        return 0;
    }

    int a = 3;
    int b = 3;
    int lo = 2;
    int hi = 9;
    int mid = 5;
    int below = 0;
    int above = 11;
    const int& mn = std::min(a, b);
    const int& mx = std::max(a, b);
    const int& cmid = std::clamp(mid, lo, hi);
    const int& clo = std::clamp(below, lo, hi);
    const int& chi = std::clamp(above, lo, hi);
    const int wide_min = std::min(INT_MIN, INT_MAX);
    const int wide_max = std::max(INT_MIN, INT_MAX);
    Ord p{1};
    Ord q{1};
    Ord r{2};
    const Ord& op = std::min(p, q);
    const Ord& oq = std::max(p, r);
    int arr[2] = {1, 2};
    const int* pa = &arr[0];
    const int* pb = &arr[0];
    const int* const& pm = std::min(pa, pb);

    std::printf("min_eq=%d\n", same_int(mn, a));
    std::printf("max_eq=%d\n", same_int(mx, a));
    std::printf("min_lo=%d\n", std::min(hi, lo));
    std::printf("max_hi=%d\n", std::max(lo, hi));
    std::printf("clamp_mid=%d id=%d\n", cmid, same_int(cmid, mid));
    std::printf("clamp_lo=%d id=%d\n", clo, same_int(clo, lo));
    std::printf("clamp_hi=%d id=%d\n", chi, same_int(chi, hi));
    std::printf("wide_min_is0=%d wide_min_is_min=%d\n", wide_min == 0 ? 1 : 0, wide_min == INT_MIN ? 1 : 0);
    std::printf("wide_max_is0=%d wide_max_is_max=%d\n", wide_max == 0 ? 1 : 0, wide_max == INT_MAX ? 1 : 0);
    std::printf("clamp_inside_zero=%d\n", std::clamp(0, INT_MIN, INT_MAX) == 0 ? 1 : 0);
    std::printf("ord_eq_first=%d ord_max=%d\n", &op == &p ? 1 : 0, oq.x);
    std::printf("ptr_eq_first=%d\n", &pm == &pa ? 1 : 0);
    return 0;
}
