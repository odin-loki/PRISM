// Differential trace of <algorithm> (tests/cpp/test_cxx_algorithm.cpp).
// Compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/algorithm) first on the include path.
// min and max return one of the two arguments. Equal arguments return the
// first. clamp of a valid range returns lo, hi, or v. min of the two
// extreme ints is the lower one, not 0. min_element and max_element take
// two pointers. An empty range returns last and does not read an element.
// Equal elements keep the first. find takes two pointers and a const T&.
// An empty range returns last and does not read. The result is the first
// operator== match, or last. sort, copy, fill, and for_each are not called.
// argv "invert" calls clamp with hi < lo. argv "invert-min" and
// "invert-max" call min_element and max_element with last < first.
// argv "invert-find" calls find with last < first. The model must
// __prism_check and must not print a value.
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

// No default constructor: an empty range must not invent a T.
struct NoDefault {
    int x;
    constexpr explicit NoDefault(int v) : x(v) {}
    NoDefault() = delete;
    friend constexpr bool operator<(const NoDefault& p, const NoDefault& q) { return p.x < q.x; }
};

// Equality only. find does not require operator<.
struct EqOnly {
    int x;
    friend constexpr bool operator==(const EqOnly& p, const EqOnly& q) { return p.x == q.x; }
};

// No default constructor: find must not invent a T.
struct NoDefaultEq {
    int x;
    constexpr explicit NoDefaultEq(int v) : x(v) {}
    NoDefaultEq() = delete;
    friend constexpr bool operator==(const NoDefaultEq& p, const NoDefaultEq& q) { return p.x == q.x; }
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

constexpr int kSeq[] = {9, 4, 1, 4};
static_assert(std::min_element(kSeq, kSeq + 4) == kSeq + 2);
static_assert(*std::min_element(kSeq, kSeq + 4) == 1);
static_assert(std::max_element(kSeq, kSeq + 4) == kSeq);
static_assert(*std::max_element(kSeq, kSeq + 4) == 9);
static_assert(std::min_element(kSeq, kSeq) == kSeq);
static_assert(std::max_element(kSeq, kSeq) == kSeq);
constexpr int kTies[] = {2, 2};
static_assert(std::min_element(kTies, kTies + 2) == kTies);
static_assert(std::max_element(kTies, kTies + 2) == kTies);
static_assert(std::min_element(static_cast<int*>(nullptr), static_cast<int*>(nullptr)) == nullptr);
static_assert(std::max_element(static_cast<const int*>(nullptr), static_cast<const int*>(nullptr)) == nullptr);
constexpr NoDefault kNd[] = {NoDefault(3), NoDefault(1), NoDefault(3)};
static_assert(std::min_element(kNd, kNd + 3)->x == 1);
static_assert(std::max_element(kNd, kNd + 3) == kNd);
static_assert(std::min_element(kNd, kNd) == kNd);
static_assert(std::max_element(kNd, kNd) == kNd);
constexpr Ord kOrd[] = {Ord{2}, Ord{2}, Ord{1}};
static_assert(std::min_element(kOrd, kOrd + 3)->x == 1);
static_assert(std::max_element(kOrd, kOrd + 3) == kOrd);

// Local arrays are non-const, so T is the element type. A constexpr array
// decays to const T* and does not match this overload.
constexpr int find_hit_i() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<int>(std::find(seq, seq + 4, 1) - seq);
}
static_assert(find_hit_i() == 2);
constexpr int find_first_i() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<int>(std::find(seq, seq + 4, 4) - seq);
}
static_assert(find_first_i() == 1);
constexpr int find_front_i() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<int>(std::find(seq, seq + 4, 9) - seq);
}
static_assert(find_front_i() == 0);
constexpr bool find_miss() {
    int seq[] = {9, 4, 1, 4};
    return std::find(seq, seq + 4, 7) == seq + 4;
}
static_assert(find_miss());
constexpr bool find_empty() {
    int seq[] = {9, 4, 1, 4};
    return std::find(seq, seq, 9) == seq;
}
static_assert(find_empty());
constexpr bool find_tie() {
    int seq[] = {2, 2};
    return std::find(seq, seq + 2, 2) == seq;
}
static_assert(find_tie());
static_assert(std::find(static_cast<int*>(nullptr), static_cast<int*>(nullptr), 0) == nullptr);
constexpr int find_eq_i() {
    EqOnly seq[] = {EqOnly{2}, EqOnly{1}, EqOnly{1}};
    EqOnly needle{1};
    EqOnly miss{3};
    if (std::find(seq, seq + 3, miss) != seq + 3) return -1;
    if (std::find(seq, seq, needle) != seq) return -1;
    return static_cast<int>(std::find(seq, seq + 3, needle) - seq);
}
static_assert(find_eq_i() == 1);
constexpr int find_nodef_i() {
    NoDefaultEq seq[] = {NoDefaultEq(3), NoDefaultEq(1), NoDefaultEq(3)};
    NoDefaultEq needle(1);
    if (std::find(seq, seq, needle) != seq) return -1;
    return std::find(seq, seq + 3, needle)->x;
}
static_assert(find_nodef_i() == 1);

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
    if (argc > 1 && (std::strcmp(argv[1], "invert-min") == 0 || std::strcmp(argv[1], "invert-max") == 0)) {
        int arr_el[] = {1, 2, 3};
        int* volatile vf = arr_el + 2;
        int* volatile vl = arr_el;
        int* first = vf;
        int* last = vl;
        int* p = std::strcmp(argv[1], "invert-min") == 0 ? std::min_element(first, last)
                                                         : std::max_element(first, last);
        std::printf("value=%d\n", *p);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "invert-find") == 0) {
        int arr_el[] = {1, 2, 3};
        int* volatile vf = arr_el + 2;
        int* volatile vl = arr_el;
        int* first = vf;
        int* last = vl;
        int needle = 1;
        int* p = std::find(first, last, needle);
        std::printf("value=%d\n", *p);
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

    int seq[] = {9, 4, 1, 4};
    int* smin = std::min_element(seq, seq + 4);
    int* smax = std::max_element(seq, seq + 4);
    std::printf("el_min=%d el_min_i=%d\n", *smin, static_cast<int>(smin - seq));
    std::printf("el_max=%d el_max_i=%d\n", *smax, static_cast<int>(smax - seq));
    int* sempty = std::min_element(seq, seq);
    int* sempty_max = std::max_element(seq, seq);
    std::printf("el_empty=%d\n", (sempty == seq && sempty_max == seq) ? 1 : 0);
    int ties[] = {2, 2};
    std::printf("el_tie=%d\n",
                (std::min_element(ties, ties + 2) == ties && std::max_element(ties, ties + 2) == ties) ? 1 : 0);
    int one[] = {8};
    std::printf("el_one=%d\n", (*std::min_element(one, one + 1) == 8 && *std::max_element(one, one + 1) == 8) ? 1 : 0);
    int* zn = nullptr;
    std::printf("el_null=%d\n", (std::min_element(zn, zn) == nullptr && std::max_element(zn, zn) == nullptr) ? 1 : 0);
    Ord os[3] = {{2}, {2}, {1}};
    const Ord* omin = std::min_element(os, os + 3);
    const Ord* omax = std::max_element(os, os + 3);
    std::printf("el_ord_min=%d el_ord_max_i=%d\n", omin->x, static_cast<int>(omax - os));
    NoDefault nds[] = {NoDefault(3), NoDefault(1), NoDefault(3)};
    const NoDefault* ndmin = std::min_element(nds, nds + 3);
    const NoDefault* ndmax = std::max_element(nds, nds + 3);
    const NoDefault* ndempty = std::min_element(nds, nds);
    const NoDefault* ndempty_max = std::max_element(nds, nds);
    std::printf("el_nodef_min=%d el_nodef_max_i=%d el_nodef_empty=%d\n", ndmin->x, static_cast<int>(ndmax - nds),
                (ndempty == nds && ndempty_max == nds) ? 1 : 0);

    int* fhit = std::find(seq, seq + 4, 1);
    int* ffirst = std::find(seq, seq + 4, 4);
    int* ffront = std::find(seq, seq + 4, 9);
    int* fmiss = std::find(seq, seq + 4, 7);
    int* fempty = std::find(seq, seq, 9);
    const int kept[] = {9, 4, 1, 4};
    std::printf("find_hit=%d find_hit_i=%d\n", *fhit, static_cast<int>(fhit - seq));
    std::printf("find_first_i=%d\n", static_cast<int>(ffirst - seq));
    std::printf("find_front_i=%d\n", static_cast<int>(ffront - seq));
    std::printf("find_miss=%d\n", fmiss == seq + 4 ? 1 : 0);
    std::printf("find_empty=%d\n", fempty == seq ? 1 : 0);
    int one_find[] = {8};
    std::printf("find_one=%d\n", (std::find(one_find, one_find + 1, 8) == one_find &&
                                  std::find(one_find, one_find + 1, 0) == one_find + 1)
                                     ? 1
                                     : 0);
    std::printf("find_null=%d\n", std::find(zn, zn, 0) == nullptr ? 1 : 0);
    std::printf("find_keep=%d\n", std::memcmp(seq, kept, sizeof seq) == 0 ? 1 : 0);
    EqOnly eqs[] = {{2}, {1}, {1}};
    EqOnly want{1};
    EqOnly miss{9};
    const EqOnly* eh = std::find(eqs, eqs + 3, want);
    const EqOnly* em = std::find(eqs, eqs + 3, miss);
    const EqOnly* ee = std::find(eqs, eqs, want);
    std::printf("find_eq_i=%d find_eq_id=%d find_eq_miss=%d find_eq_empty=%d\n", static_cast<int>(eh - eqs),
                eh != &want ? 1 : 0, em == eqs + 3 ? 1 : 0, ee == eqs ? 1 : 0);
    NoDefaultEq ndes[] = {NoDefaultEq(3), NoDefaultEq(1), NoDefaultEq(3)};
    NoDefaultEq ndneedle(1);
    const NoDefaultEq* ndh = std::find(ndes, ndes + 3, ndneedle);
    const NoDefaultEq* nde = std::find(ndes, ndes, ndneedle);
    std::printf("find_nodef=%d find_nodef_i=%d find_nodef_empty=%d\n", ndh->x, static_cast<int>(ndh - ndes),
                nde == ndes ? 1 : 0);
    return 0;
}
