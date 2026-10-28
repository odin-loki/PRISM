// Differential trace of <algorithm> (tests/cpp/test_cxx_algorithm.cpp).
// Compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/algorithm) first on the include path.
// min and max return one of the two arguments. Equal arguments return the
// first. clamp of a valid range returns lo, hi, or v. min of the two
// extreme ints is the lower one, not 0. min_element and max_element take
// two pointers. An empty range returns last and does not read an element.
// Equal elements keep the first. find takes two pointers and a const T&.
// An empty range returns last and does not read. The result is the first
// operator== match, or last. find_if takes two pointers and
// bool (*)(const T&). An empty range returns last and does not call the
// predicate. count returns how many elements compare equal. An empty range
// returns 0 and does not read. all_of, any_of, and none_of take two
// pointers and bool (*)(const T&). An empty range does not call: all_of
// returns true, any_of returns false, none_of returns true. fill takes two
// pointers and a const T&. An empty range does not write, even when the
// pointer is null. Each element in [first, last) becomes the value. The
// element at last is left unchanged. copy takes two pointers and a
// destination pointer. An empty range returns dest and does not read or
// write, even when dest is null. Each element in [first, last) is copied.
// The element at last is left unchanged. The result is one past the last
// element written. sort, copy_n, copy_backward, and for_each are not
// called.
// argv "invert" calls clamp with hi < lo. argv "invert-min" and
// "invert-max" call min_element and max_element with last < first.
// argv "invert-find" calls find with last < first. argv "invert-find-if"
// calls find_if with last < first and must not call the predicate.
// argv "null-find-if" and "null-find-if-empty" pass a null predicate.
// argv "invert-count" calls count with last < first. argv "invert-all-of",
// "invert-any-of", and "invert-none-of" call those functions with
// last < first and must not call the predicate. argv "null-all-of",
// "null-any-of", "null-none-of", and the "-empty" forms pass a null
// predicate. argv "invert-fill" calls fill with last < first and must not
// assign. argv "null-fill" calls fill with a null first and a non-empty
// range and must not assign. argv "invert-copy" calls copy with last < first
// and must not read or write. argv "null-copy" calls copy with a null dest
// and a non-empty range and must not write. argv "overlap-copy" and
// "overlap-copy-first" call copy with dest inside [first, last) and must
// not read or write. The model must __prism_check and must not print a
// value. This is not PROVED and not CLEAN.
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

// No operator== and no default constructor. find_if must not invent a T.
struct PredOnly {
    int x;
    constexpr explicit PredOnly(int v) : x(v) {}
    PredOnly() = delete;
};

// Assignment is the write fill must not perform on a rejected range.
struct FillSlot {
    int x;
    FillSlot& operator=(const FillSlot& other) {
        std::fprintf(stderr, "wrote\n");
        x = other.x;
        return *this;
    }
};

// Assignment is the write copy must not perform on a rejected range.
// Equality of elements is the read the overlap scan must not perform.
struct CopySlot {
    int x;
    CopySlot& operator=(const CopySlot& other) {
        std::fprintf(stderr, "wrote\n");
        x = other.x;
        return *this;
    }
    friend bool operator==(const CopySlot& a, const CopySlot& b) {
        std::fprintf(stderr, "read\n");
        return a.x == b.x;
    }
};

constexpr bool is_one(const int& x) { return x == 1; }
constexpr bool is_four(const int& x) { return x == 4; }
constexpr bool is_nine(const int& x) { return x == 9; }
constexpr bool is_seven(const int& x) { return x == 7; }
constexpr bool is_two(const int& x) { return x == 2; }
constexpr bool eq_is_one(const EqOnly& e) { return e.x == 1; }
constexpr bool nd_is_one(const NoDefault& n) { return n.x == 1; }
constexpr bool pred_only_is_one(const PredOnly& p) { return p.x == 1; }

int g_calls = 0;
int g_want = 0;
bool watch(const int& x) {
    ++g_calls;
    return x == g_want;
}
bool watch_true(const int&) {
    ++g_calls;
    return true;
}
bool watch_false(const int&) {
    ++g_calls;
    return false;
}
bool mark_called(const int&) {
    std::fprintf(stderr, "called\n");
    return true;
}

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

constexpr int find_if_hit_i() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<int>(std::find_if(seq, seq + 4, is_one) - seq);
}
static_assert(find_if_hit_i() == 2);
constexpr int find_if_first_i() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<int>(std::find_if(seq, seq + 4, is_four) - seq);
}
static_assert(find_if_first_i() == 1);
constexpr int find_if_front_i() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<int>(std::find_if(seq, seq + 4, is_nine) - seq);
}
static_assert(find_if_front_i() == 0);
constexpr bool find_if_miss() {
    int seq[] = {9, 4, 1, 4};
    return std::find_if(seq, seq + 4, is_seven) == seq + 4;
}
static_assert(find_if_miss());
constexpr bool find_if_empty() {
    int seq[] = {9, 4, 1, 4};
    return std::find_if(seq, seq, is_nine) == seq;
}
static_assert(find_if_empty());
constexpr bool find_if_tie() {
    int seq[] = {2, 2};
    return std::find_if(seq, seq + 2, is_two) == seq;
}
static_assert(find_if_tie());
static_assert(std::find_if(static_cast<int*>(nullptr), static_cast<int*>(nullptr), is_one) == nullptr);
constexpr int find_if_eq_i() {
    EqOnly seq[] = {EqOnly{2}, EqOnly{1}, EqOnly{1}};
    if (std::find_if(seq, seq, eq_is_one) != seq) return -1;
    return static_cast<int>(std::find_if(seq, seq + 3, eq_is_one) - seq);
}
static_assert(find_if_eq_i() == 1);
constexpr int find_if_nodef_i() {
    NoDefault seq[] = {NoDefault(3), NoDefault(1), NoDefault(3)};
    if (std::find_if(seq, seq, nd_is_one) != seq) return -1;
    return static_cast<int>(std::find_if(seq, seq + 3, nd_is_one) - seq);
}
static_assert(find_if_nodef_i() == 1);
constexpr int find_if_pred_i() {
    PredOnly seq[] = {PredOnly(3), PredOnly(1), PredOnly(3)};
    if (std::find_if(seq, seq, pred_only_is_one) != seq) return -1;
    return static_cast<int>(std::find_if(seq, seq + 3, pred_only_is_one) - seq);
}
static_assert(find_if_pred_i() == 1);

constexpr long long count_four() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<long long>(std::count(seq, seq + 4, 4));
}
static_assert(count_four() == 2);
constexpr long long count_one() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<long long>(std::count(seq, seq + 4, 1));
}
static_assert(count_one() == 1);
constexpr long long count_nine() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<long long>(std::count(seq, seq + 4, 9));
}
static_assert(count_nine() == 1);
constexpr long long count_miss() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<long long>(std::count(seq, seq + 4, 7));
}
static_assert(count_miss() == 0);
constexpr long long count_empty() {
    int seq[] = {9, 4, 1, 4};
    return static_cast<long long>(std::count(seq, seq, 9));
}
static_assert(count_empty() == 0);
static_assert(std::count(static_cast<int*>(nullptr), static_cast<int*>(nullptr), 0) == 0);
constexpr long long count_eq() {
    EqOnly seq[] = {EqOnly{2}, EqOnly{1}, EqOnly{1}};
    EqOnly needle{1};
    EqOnly miss{9};
    if (std::count(seq, seq, needle) != 0) return -1;
    if (std::count(seq, seq + 3, miss) != 0) return -1;
    return static_cast<long long>(std::count(seq, seq + 3, needle));
}
static_assert(count_eq() == 2);
constexpr long long count_nodef() {
    NoDefaultEq seq[] = {NoDefaultEq(3), NoDefaultEq(1), NoDefaultEq(3)};
    NoDefaultEq needle(1);
    NoDefaultEq three(3);
    if (std::count(seq, seq, needle) != 0) return -1;
    if (std::count(seq, seq + 3, three) != 2) return -1;
    return static_cast<long long>(std::count(seq, seq + 3, needle));
}
static_assert(count_nodef() == 1);

constexpr bool all_of_all() {
    int seq[] = {1, 1, 1};
    return std::all_of(seq, seq + 3, is_one);
}
static_assert(all_of_all());
constexpr bool all_of_front() {
    int seq[] = {9, 4, 1, 4};
    return std::all_of(seq, seq + 4, is_four);
}
static_assert(!all_of_front());
constexpr bool all_of_mid() {
    int seq[] = {9, 9, 1, 4};
    return std::all_of(seq, seq + 4, is_nine);
}
static_assert(!all_of_mid());
constexpr bool all_of_empty() {
    int seq[] = {9, 4, 1, 4};
    return std::all_of(seq, seq, is_nine);
}
static_assert(all_of_empty());
constexpr bool all_of_one_true() {
    int seq[] = {1};
    return std::all_of(seq, seq + 1, is_one);
}
static_assert(all_of_one_true());
constexpr bool all_of_one_false() {
    int seq[] = {8};
    return std::all_of(seq, seq + 1, is_one);
}
static_assert(!all_of_one_false());
static_assert(std::all_of(static_cast<int*>(nullptr), static_cast<int*>(nullptr), is_one));
constexpr bool all_of_eq_empty() {
    EqOnly seq[] = {EqOnly{2}, EqOnly{1}, EqOnly{1}};
    if (std::all_of(seq, seq + 3, eq_is_one)) return false;
    return std::all_of(seq, seq, eq_is_one);
}
static_assert(all_of_eq_empty());
constexpr bool all_of_nodef_empty() {
    NoDefault seq[] = {NoDefault(3), NoDefault(1), NoDefault(3)};
    if (std::all_of(seq, seq + 3, nd_is_one)) return false;
    return std::all_of(seq, seq, nd_is_one);
}
static_assert(all_of_nodef_empty());
constexpr bool all_of_pred_empty() {
    PredOnly seq[] = {PredOnly(3), PredOnly(1), PredOnly(3)};
    if (std::all_of(seq, seq + 3, pred_only_is_one)) return false;
    return std::all_of(seq, seq, pred_only_is_one);
}
static_assert(all_of_pred_empty());

constexpr bool any_of_hit() {
    int seq[] = {9, 4, 1, 4};
    return std::any_of(seq, seq + 4, is_one);
}
static_assert(any_of_hit());
constexpr bool any_of_front() {
    int seq[] = {9, 4, 1, 4};
    return std::any_of(seq, seq + 4, is_nine);
}
static_assert(any_of_front());
constexpr bool any_of_miss() {
    int seq[] = {9, 4, 1, 4};
    return std::any_of(seq, seq + 4, is_seven);
}
static_assert(!any_of_miss());
constexpr bool any_of_empty() {
    int seq[] = {9, 4, 1, 4};
    return std::any_of(seq, seq, is_nine);
}
static_assert(!any_of_empty());
constexpr bool any_of_tie() {
    int seq[] = {2, 2};
    return std::any_of(seq, seq + 2, is_two);
}
static_assert(any_of_tie());
static_assert(!std::any_of(static_cast<int*>(nullptr), static_cast<int*>(nullptr), is_one));
constexpr bool any_of_eq() {
    EqOnly seq[] = {EqOnly{2}, EqOnly{1}, EqOnly{1}};
    if (std::any_of(seq, seq, eq_is_one)) return false;
    return std::any_of(seq, seq + 3, eq_is_one);
}
static_assert(any_of_eq());
constexpr bool any_of_nodef() {
    NoDefault seq[] = {NoDefault(3), NoDefault(1), NoDefault(3)};
    if (std::any_of(seq, seq, nd_is_one)) return false;
    return std::any_of(seq, seq + 3, nd_is_one);
}
static_assert(any_of_nodef());
constexpr bool any_of_pred() {
    PredOnly seq[] = {PredOnly(3), PredOnly(1), PredOnly(3)};
    if (std::any_of(seq, seq, pred_only_is_one)) return false;
    return std::any_of(seq, seq + 3, pred_only_is_one);
}
static_assert(any_of_pred());

constexpr bool none_of_miss() {
    int seq[] = {9, 4, 1, 4};
    return std::none_of(seq, seq + 4, is_seven);
}
static_assert(none_of_miss());
constexpr bool none_of_front() {
    int seq[] = {9, 4, 1, 4};
    return std::none_of(seq, seq + 4, is_nine);
}
static_assert(!none_of_front());
constexpr bool none_of_mid() {
    int seq[] = {9, 4, 1, 4};
    return std::none_of(seq, seq + 4, is_one);
}
static_assert(!none_of_mid());
constexpr bool none_of_empty() {
    int seq[] = {9, 4, 1, 4};
    return std::none_of(seq, seq, is_nine);
}
static_assert(none_of_empty());
static_assert(std::none_of(static_cast<int*>(nullptr), static_cast<int*>(nullptr), is_one));
constexpr bool none_of_eq_empty() {
    EqOnly seq[] = {EqOnly{2}, EqOnly{1}, EqOnly{1}};
    if (std::none_of(seq, seq + 3, eq_is_one)) return false;
    return std::none_of(seq, seq, eq_is_one);
}
static_assert(none_of_eq_empty());
constexpr bool none_of_nodef_empty() {
    NoDefault seq[] = {NoDefault(3), NoDefault(1), NoDefault(3)};
    if (std::none_of(seq, seq + 3, nd_is_one)) return false;
    return std::none_of(seq, seq, nd_is_one);
}
static_assert(none_of_nodef_empty());
constexpr bool none_of_pred_empty() {
    PredOnly seq[] = {PredOnly(3), PredOnly(1), PredOnly(3)};
    if (std::none_of(seq, seq + 3, pred_only_is_one)) return false;
    return std::none_of(seq, seq, pred_only_is_one);
}
static_assert(none_of_pred_empty());

constexpr int fill_sum() {
    int seq[] = {9, 4, 1, 4};
    std::fill(seq, seq + 4, 7);
    return seq[0] + seq[1] + seq[2] + seq[3];
}
static_assert(fill_sum() == 28);
constexpr int fill_tail() {
    int seq[] = {9, 4, 1, 4};
    std::fill(seq, seq + 3, 7);
    return seq[0] + seq[1] * 10 + seq[2] * 100 + seq[3] * 1000;
}
static_assert(fill_tail() == 4777);
constexpr bool fill_empty_keeps() {
    int seq[] = {9, 4, 1, 4};
    std::fill(seq, seq, 0);
    return seq[0] == 9 && seq[1] == 4 && seq[2] == 1 && seq[3] == 4;
}
static_assert(fill_empty_keeps());
constexpr bool fill_null_empty() {
    std::fill(static_cast<int*>(nullptr), static_cast<int*>(nullptr), 0);
    return true;
}
static_assert(fill_null_empty());
constexpr int fill_one_v() {
    int seq[] = {8};
    std::fill(seq, seq + 1, 3);
    return seq[0];
}
static_assert(fill_one_v() == 3);
constexpr int fill_pred_v() {
    PredOnly seq[] = {PredOnly(3), PredOnly(1), PredOnly(3)};
    PredOnly value(9);
    std::fill(seq, seq + 3, value);
    PredOnly empty[] = {PredOnly(4), PredOnly(5)};
    std::fill(empty, empty, PredOnly(8));
    return seq[0].x + seq[1].x + seq[2].x + empty[0].x + empty[1].x;
}
static_assert(fill_pred_v() == 36);

constexpr int copy_sum() {
    int seq[] = {9, 4, 1, 4};
    int out[] = {0, 0, 0, 0};
    int* end = std::copy(seq, seq + 4, out);
    if (end != out + 4) return -1;
    if (seq[0] != 9 || seq[3] != 4) return -1;
    return out[0] + out[1] + out[2] + out[3];
}
static_assert(copy_sum() == 18);
constexpr int copy_tail() {
    int seq[] = {9, 4, 1, 4};
    int out[] = {8, 8, 8, 8};
    int* end = std::copy(seq, seq + 3, out);
    if (end != out + 3) return -1;
    if (seq[3] != 4 || out[3] != 8) return -1;
    return out[0] * 1000 + out[1] * 100 + out[2] * 10 + out[3];
}
static_assert(copy_tail() == 9418);
constexpr bool copy_empty_keeps() {
    int seq[] = {9, 4, 1, 4};
    int out[] = {8, 7, 6, 5};
    int* end = std::copy(seq, seq, out);
    return end == out && out[0] == 8 && out[1] == 7 && out[2] == 6 && out[3] == 5 && seq[0] == 9;
}
static_assert(copy_empty_keeps());
constexpr bool copy_null_empty() {
    int* end = std::copy(static_cast<int*>(nullptr), static_cast<int*>(nullptr), static_cast<int*>(nullptr));
    return end == nullptr;
}
static_assert(copy_null_empty());
constexpr int copy_one_v() {
    int seq[] = {8};
    int out[] = {0};
    int* end = std::copy(seq, seq + 1, out);
    return (end == out + 1 && out[0] == 8 && seq[0] == 8) ? 8 : 0;
}
static_assert(copy_one_v() == 8);
constexpr int copy_pred_v() {
    PredOnly seq[] = {PredOnly(3), PredOnly(1), PredOnly(3)};
    PredOnly out[] = {PredOnly(0), PredOnly(0), PredOnly(0), PredOnly(5)};
    PredOnly* end = std::copy(seq, seq + 3, out);
    PredOnly empty_src[] = {PredOnly(4), PredOnly(5)};
    PredOnly empty_dst[] = {PredOnly(4), PredOnly(5)};
    PredOnly* end_empty = std::copy(empty_src, empty_src, empty_dst);
    if (end != out + 3 || end_empty != empty_dst) return -1;
    if (empty_dst[0].x != 4 || empty_dst[1].x != 5) return -1;
    if (seq[0].x != 3 || seq[1].x != 1 || seq[2].x != 3) return -1;
    if (out[3].x != 5) return -1;
    return out[0].x + out[1].x + out[2].x + out[3].x;
}
static_assert(copy_pred_v() == 12);
constexpr bool copy_adjacent() {
    int seq[] = {1, 2, 3, 0, 0, 0};
    int* end = std::copy(seq, seq + 3, seq + 3);
    return end == seq + 6 && seq[0] == 1 && seq[1] == 2 && seq[2] == 3 && seq[3] == 1 && seq[4] == 2 && seq[5] == 3;
}
static_assert(copy_adjacent());

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
    if (argc > 1 && std::strcmp(argv[1], "invert-find-if") == 0) {
        int arr_el[] = {1, 2, 3};
        int* volatile vf = arr_el + 2;
        int* volatile vl = arr_el;
        int* first = vf;
        int* last = vl;
        int* p = std::find_if(first, last, mark_called);
        std::printf("value=%d\n", *p);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "null-find-if") == 0) {
        int arr_el[] = {1, 2, 3};
        int* p = std::find_if(arr_el, arr_el + 3, static_cast<bool (*)(const int&)>(nullptr));
        std::printf("value=%d\n", *p);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "null-find-if-empty") == 0) {
        int arr_el[] = {1, 2, 3};
        int* p = std::find_if(arr_el, arr_el, static_cast<bool (*)(const int&)>(nullptr));
        std::printf("value=%d\n", p == arr_el ? 1 : 0);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "invert-count") == 0) {
        int arr_el[] = {1, 2, 3};
        int* volatile vf = arr_el + 2;
        int* volatile vl = arr_el;
        int* first = vf;
        int* last = vl;
        long long n = static_cast<long long>(std::count(first, last, 1));
        std::printf("value=%lld\n", n);
        return 0;
    }
    if (argc > 1 && (std::strcmp(argv[1], "invert-all-of") == 0 || std::strcmp(argv[1], "invert-any-of") == 0 ||
                     std::strcmp(argv[1], "invert-none-of") == 0)) {
        int arr_el[] = {1, 2, 3};
        int* volatile vf = arr_el + 2;
        int* volatile vl = arr_el;
        int* first = vf;
        int* last = vl;
        bool r = false;
        if (std::strcmp(argv[1], "invert-all-of") == 0) r = std::all_of(first, last, mark_called);
        else if (std::strcmp(argv[1], "invert-any-of") == 0) r = std::any_of(first, last, mark_called);
        else r = std::none_of(first, last, mark_called);
        std::printf("value=%d\n", r ? 1 : 0);
        return 0;
    }
    if (argc > 1 && (std::strcmp(argv[1], "null-all-of") == 0 || std::strcmp(argv[1], "null-all-of-empty") == 0 ||
                     std::strcmp(argv[1], "null-any-of") == 0 || std::strcmp(argv[1], "null-any-of-empty") == 0 ||
                     std::strcmp(argv[1], "null-none-of") == 0 || std::strcmp(argv[1], "null-none-of-empty") == 0)) {
        int arr_el[] = {1, 2, 3};
        const bool empty = std::strcmp(argv[1], "null-all-of-empty") == 0 ||
                           std::strcmp(argv[1], "null-any-of-empty") == 0 ||
                           std::strcmp(argv[1], "null-none-of-empty") == 0;
        int* first = arr_el;
        int* last = empty ? arr_el : arr_el + 3;
        auto pred = static_cast<bool (*)(const int&)>(nullptr);
        bool r = false;
        if (std::strcmp(argv[1], "null-all-of") == 0 || std::strcmp(argv[1], "null-all-of-empty") == 0)
            r = std::all_of(first, last, pred);
        else if (std::strcmp(argv[1], "null-any-of") == 0 || std::strcmp(argv[1], "null-any-of-empty") == 0)
            r = std::any_of(first, last, pred);
        else
            r = std::none_of(first, last, pred);
        std::printf("value=%d\n", r ? 1 : 0);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "invert-fill") == 0) {
        FillSlot arr_el[3] = {{1}, {2}, {3}};
        FillSlot* volatile vf = arr_el + 2;
        FillSlot* volatile vl = arr_el;
        FillSlot* first = vf;
        FillSlot* last = vl;
        FillSlot value{9};
        std::fill(first, last, value);
        std::printf("value=%d\n", arr_el[0].x);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "null-fill") == 0) {
        unsigned long volatile raw_first = 0;
        unsigned long volatile raw_last = sizeof(FillSlot);
        FillSlot* first = reinterpret_cast<FillSlot*>(raw_first);
        FillSlot* last = reinterpret_cast<FillSlot*>(raw_last);
        FillSlot value{9};
        std::fill(first, last, value);
        std::printf("value=%d\n", value.x);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "invert-copy") == 0) {
        CopySlot src[3] = {{1}, {2}, {3}};
        CopySlot dst[3] = {{7}, {7}, {7}};
        CopySlot* volatile vf = src + 2;
        CopySlot* volatile vl = src;
        CopySlot* first = vf;
        CopySlot* last = vl;
        CopySlot* end = std::copy(first, last, dst);
        std::printf("value=%d\n", end == dst ? 1 : 0);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "null-copy") == 0) {
        CopySlot src[3] = {{1}, {2}, {3}};
        unsigned long volatile raw_dest = 0;
        CopySlot* dest = reinterpret_cast<CopySlot*>(raw_dest);
        CopySlot* end = std::copy(src, src + 3, dest);
        std::printf("value=%d\n", end == nullptr ? 1 : 0);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "overlap-copy") == 0) {
        CopySlot arr[4] = {{1}, {2}, {3}, {4}};
        CopySlot* volatile vd = arr + 2;
        CopySlot* dest = vd;
        CopySlot* end = std::copy(arr, arr + 3, dest);
        std::printf("value=%d\n", static_cast<int>(end - arr));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "overlap-copy-first") == 0) {
        CopySlot arr[3] = {{1}, {2}, {3}};
        CopySlot* end = std::copy(arr, arr + 3, arr);
        std::printf("value=%d\n", end == arr ? arr[0].x : -1);
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

    g_calls = 0;
    g_want = 1;
    int* ifhit = std::find_if(seq, seq + 4, watch);
    int ifhit_calls = g_calls;
    g_calls = 0;
    g_want = 4;
    int* iffirst = std::find_if(seq, seq + 4, watch);
    int iffirst_calls = g_calls;
    g_calls = 0;
    g_want = 9;
    int* iffront = std::find_if(seq, seq + 4, watch);
    int iffront_calls = g_calls;
    g_calls = 0;
    g_want = 7;
    int* ifmiss = std::find_if(seq, seq + 4, watch);
    int ifmiss_calls = g_calls;
    g_calls = 0;
    g_want = 9;
    int* ifempty = std::find_if(seq, seq, watch);
    int ifempty_calls = g_calls;
    g_calls = 0;
    g_want = 2;
    int* iftie = std::find_if(ties, ties + 2, watch);
    int iftie_calls = g_calls;
    std::printf("find_if_hit=%d find_if_hit_i=%d find_if_hit_calls=%d\n", *ifhit, static_cast<int>(ifhit - seq),
                ifhit_calls);
    std::printf("find_if_first_i=%d find_if_first_calls=%d\n", static_cast<int>(iffirst - seq), iffirst_calls);
    std::printf("find_if_front_i=%d find_if_front_calls=%d\n", static_cast<int>(iffront - seq), iffront_calls);
    std::printf("find_if_miss=%d find_if_miss_calls=%d\n", ifmiss == seq + 4 ? 1 : 0, ifmiss_calls);
    std::printf("find_if_empty=%d find_if_empty_calls=%d\n", ifempty == seq ? 1 : 0, ifempty_calls);
    std::printf("find_if_tie=%d find_if_tie_calls=%d\n", (iftie == ties && iftie_calls == 1) ? 1 : 0, iftie_calls);
    g_calls = 0;
    g_want = 8;
    int* ifone = std::find_if(one, one + 1, watch);
    int ifone_calls = g_calls;
    g_calls = 0;
    g_want = 0;
    int* ifone_miss = std::find_if(one, one + 1, watch);
    std::printf("find_if_one=%d\n", (ifone == one && ifone_calls == 1 && ifone_miss == one + 1 && g_calls == 1) ? 1 : 0);
    g_calls = 0;
    g_want = 1;
    std::printf("find_if_null=%d\n", (std::find_if(zn, zn, watch) == nullptr && g_calls == 0) ? 1 : 0);
    std::printf("find_if_keep=%d\n", std::memcmp(seq, kept, sizeof seq) == 0 ? 1 : 0);
    g_calls = 0;
    const EqOnly* ifeq = std::find_if(eqs, eqs + 3, eq_is_one);
    const EqOnly* ifeq_empty = std::find_if(eqs, eqs, eq_is_one);
    std::printf("find_if_eq_i=%d find_if_eq_empty=%d\n", static_cast<int>(ifeq - eqs), ifeq_empty == eqs ? 1 : 0);
    const NoDefault* ifnd = std::find_if(nds, nds + 3, nd_is_one);
    const NoDefault* ifnd_empty = std::find_if(nds, nds, nd_is_one);
    std::printf("find_if_nodef_i=%d find_if_nodef_empty=%d\n", static_cast<int>(ifnd - nds), ifnd_empty == nds ? 1 : 0);
    PredOnly preds[] = {PredOnly(3), PredOnly(1), PredOnly(3)};
    const PredOnly* ifpred = std::find_if(preds, preds + 3, pred_only_is_one);
    const PredOnly* ifpred_empty = std::find_if(preds, preds, pred_only_is_one);
    std::printf("find_if_pred_i=%d find_if_pred_empty=%d\n", static_cast<int>(ifpred - preds),
                ifpred_empty == preds ? 1 : 0);

    std::printf("count_four=%lld\n", static_cast<long long>(std::count(seq, seq + 4, 4)));
    std::printf("count_one=%lld\n", static_cast<long long>(std::count(seq, seq + 4, 1)));
    std::printf("count_nine=%lld\n", static_cast<long long>(std::count(seq, seq + 4, 9)));
    std::printf("count_miss=%lld\n", static_cast<long long>(std::count(seq, seq + 4, 7)));
    std::printf("count_empty=%lld\n", static_cast<long long>(std::count(seq, seq, 9)));
    std::printf("count_tie=%lld\n", static_cast<long long>(std::count(ties, ties + 2, 2)));
    std::printf("count_one_hit=%lld\n", static_cast<long long>(std::count(one, one + 1, 8)));
    std::printf("count_one_miss=%lld\n", static_cast<long long>(std::count(one, one + 1, 0)));
    std::printf("count_null=%lld\n", static_cast<long long>(std::count(zn, zn, 0)));
    EqOnly eq_miss{9};
    std::printf("count_eq=%lld count_eq_miss=%lld count_eq_empty=%lld\n",
                static_cast<long long>(std::count(eqs, eqs + 3, want)),
                static_cast<long long>(std::count(eqs, eqs + 3, eq_miss)),
                static_cast<long long>(std::count(eqs, eqs, want)));
    NoDefaultEq nd_three(3);
    std::printf("count_nodef=%lld count_nodef_three=%lld count_nodef_empty=%lld\n",
                static_cast<long long>(std::count(ndes, ndes + 3, ndneedle)),
                static_cast<long long>(std::count(ndes, ndes + 3, nd_three)),
                static_cast<long long>(std::count(ndes, ndes, ndneedle)));
    std::printf("count_keep=%d\n", std::memcmp(seq, kept, sizeof seq) == 0 ? 1 : 0);

    g_calls = 0;
    const bool ao_all = std::all_of(seq, seq + 4, watch_true);
    const int ao_all_calls = g_calls;
    g_calls = 0;
    const bool ao_front = std::all_of(seq, seq + 4, watch_false);
    const int ao_front_calls = g_calls;
    g_calls = 0;
    g_want = 9;
    const bool ao_mid = std::all_of(seq, seq + 4, watch);
    const int ao_mid_calls = g_calls;
    g_calls = 0;
    const bool ao_empty = std::all_of(seq, seq, watch_true);
    const int ao_empty_calls = g_calls;
    std::printf("all_of_all=%d all_of_all_calls=%d\n", ao_all ? 1 : 0, ao_all_calls);
    std::printf("all_of_front=%d all_of_front_calls=%d\n", ao_front ? 1 : 0, ao_front_calls);
    std::printf("all_of_mid=%d all_of_mid_calls=%d\n", ao_mid ? 1 : 0, ao_mid_calls);
    std::printf("all_of_empty=%d all_of_empty_calls=%d\n", ao_empty ? 1 : 0, ao_empty_calls);
    g_calls = 0;
    const bool ao_one_t = std::all_of(one, one + 1, watch_true);
    const int ao_one_t_calls = g_calls;
    g_calls = 0;
    const bool ao_one_f = std::all_of(one, one + 1, watch_false);
    const int ao_one_f_calls = g_calls;
    std::printf("all_of_one=%d\n", (ao_one_t && ao_one_t_calls == 1 && !ao_one_f && ao_one_f_calls == 1) ? 1 : 0);
    g_calls = 0;
    const bool ao_null = std::all_of(zn, zn, watch_true);
    std::printf("all_of_null=%d all_of_null_calls=%d\n", ao_null ? 1 : 0, g_calls);
    std::printf("all_of_keep=%d\n", std::memcmp(seq, kept, sizeof seq) == 0 ? 1 : 0);
    std::printf("all_of_eq=%d all_of_eq_empty=%d\n", std::all_of(eqs, eqs + 3, eq_is_one) ? 1 : 0,
                std::all_of(eqs, eqs, eq_is_one) ? 1 : 0);
    std::printf("all_of_nodef=%d all_of_nodef_empty=%d\n", std::all_of(nds, nds + 3, nd_is_one) ? 1 : 0,
                std::all_of(nds, nds, nd_is_one) ? 1 : 0);
    std::printf("all_of_pred=%d all_of_pred_empty=%d\n", std::all_of(preds, preds + 3, pred_only_is_one) ? 1 : 0,
                std::all_of(preds, preds, pred_only_is_one) ? 1 : 0);

    g_calls = 0;
    g_want = 1;
    const bool ay_hit = std::any_of(seq, seq + 4, watch);
    const int ay_hit_calls = g_calls;
    g_calls = 0;
    g_want = 9;
    const bool ay_front = std::any_of(seq, seq + 4, watch);
    const int ay_front_calls = g_calls;
    g_calls = 0;
    g_want = 7;
    const bool ay_miss = std::any_of(seq, seq + 4, watch);
    const int ay_miss_calls = g_calls;
    g_calls = 0;
    g_want = 9;
    const bool ay_empty = std::any_of(seq, seq, watch);
    const int ay_empty_calls = g_calls;
    g_calls = 0;
    g_want = 2;
    const bool ay_tie = std::any_of(ties, ties + 2, watch);
    const int ay_tie_calls = g_calls;
    std::printf("any_of_hit=%d any_of_hit_calls=%d\n", ay_hit ? 1 : 0, ay_hit_calls);
    std::printf("any_of_front=%d any_of_front_calls=%d\n", ay_front ? 1 : 0, ay_front_calls);
    std::printf("any_of_miss=%d any_of_miss_calls=%d\n", ay_miss ? 1 : 0, ay_miss_calls);
    std::printf("any_of_empty=%d any_of_empty_calls=%d\n", ay_empty ? 1 : 0, ay_empty_calls);
    std::printf("any_of_tie=%d any_of_tie_calls=%d\n", ay_tie ? 1 : 0, ay_tie_calls);
    g_calls = 0;
    g_want = 8;
    const bool ay_one = std::any_of(one, one + 1, watch);
    const int ay_one_calls = g_calls;
    g_calls = 0;
    g_want = 0;
    const bool ay_one_miss = std::any_of(one, one + 1, watch);
    std::printf("any_of_one=%d\n", (ay_one && ay_one_calls == 1 && !ay_one_miss && g_calls == 1) ? 1 : 0);
    g_calls = 0;
    g_want = 1;
    const bool ay_null = std::any_of(zn, zn, watch);
    std::printf("any_of_null=%d any_of_null_calls=%d\n", ay_null ? 1 : 0, g_calls);
    std::printf("any_of_keep=%d\n", std::memcmp(seq, kept, sizeof seq) == 0 ? 1 : 0);
    std::printf("any_of_eq=%d any_of_eq_empty=%d\n", std::any_of(eqs, eqs + 3, eq_is_one) ? 1 : 0,
                std::any_of(eqs, eqs, eq_is_one) ? 1 : 0);
    std::printf("any_of_nodef=%d any_of_nodef_empty=%d\n", std::any_of(nds, nds + 3, nd_is_one) ? 1 : 0,
                std::any_of(nds, nds, nd_is_one) ? 1 : 0);
    std::printf("any_of_pred=%d any_of_pred_empty=%d\n", std::any_of(preds, preds + 3, pred_only_is_one) ? 1 : 0,
                std::any_of(preds, preds, pred_only_is_one) ? 1 : 0);

    g_calls = 0;
    g_want = 7;
    const bool no_miss = std::none_of(seq, seq + 4, watch);
    const int no_miss_calls = g_calls;
    g_calls = 0;
    g_want = 9;
    const bool no_front = std::none_of(seq, seq + 4, watch);
    const int no_front_calls = g_calls;
    g_calls = 0;
    g_want = 1;
    const bool no_mid = std::none_of(seq, seq + 4, watch);
    const int no_mid_calls = g_calls;
    g_calls = 0;
    g_want = 9;
    const bool no_empty = std::none_of(seq, seq, watch);
    const int no_empty_calls = g_calls;
    std::printf("none_of_miss=%d none_of_miss_calls=%d\n", no_miss ? 1 : 0, no_miss_calls);
    std::printf("none_of_front=%d none_of_front_calls=%d\n", no_front ? 1 : 0, no_front_calls);
    std::printf("none_of_mid=%d none_of_mid_calls=%d\n", no_mid ? 1 : 0, no_mid_calls);
    std::printf("none_of_empty=%d none_of_empty_calls=%d\n", no_empty ? 1 : 0, no_empty_calls);
    g_calls = 0;
    g_want = 0;
    const bool no_one = std::none_of(one, one + 1, watch);
    const int no_one_calls = g_calls;
    g_calls = 0;
    g_want = 8;
    const bool no_one_hit = std::none_of(one, one + 1, watch);
    std::printf("none_of_one=%d\n", (no_one && no_one_calls == 1 && !no_one_hit && g_calls == 1) ? 1 : 0);
    g_calls = 0;
    g_want = 1;
    const bool no_null = std::none_of(zn, zn, watch);
    std::printf("none_of_null=%d none_of_null_calls=%d\n", no_null ? 1 : 0, g_calls);
    std::printf("none_of_keep=%d\n", std::memcmp(seq, kept, sizeof seq) == 0 ? 1 : 0);
    std::printf("none_of_eq=%d none_of_eq_empty=%d\n", std::none_of(eqs, eqs + 3, eq_is_one) ? 1 : 0,
                std::none_of(eqs, eqs, eq_is_one) ? 1 : 0);
    std::printf("none_of_nodef=%d none_of_nodef_empty=%d\n", std::none_of(nds, nds + 3, nd_is_one) ? 1 : 0,
                std::none_of(nds, nds, nd_is_one) ? 1 : 0);
    std::printf("none_of_pred=%d none_of_pred_empty=%d\n", std::none_of(preds, preds + 3, pred_only_is_one) ? 1 : 0,
                std::none_of(preds, preds, pred_only_is_one) ? 1 : 0);

    int fill_all[] = {9, 4, 1, 4};
    std::fill(fill_all, fill_all + 4, 7);
    std::printf("fill_all=%d,%d,%d,%d\n", fill_all[0], fill_all[1], fill_all[2], fill_all[3]);
    int fill_span[] = {9, 4, 1, 4};
    std::fill(fill_span, fill_span + 3, 7);
    std::printf("fill_span=%d,%d,%d,%d\n", fill_span[0], fill_span[1], fill_span[2], fill_span[3]);
    int fill_emp[] = {9, 4, 1, 4};
    std::fill(fill_emp, fill_emp, 0);
    std::printf("fill_empty=%d,%d,%d,%d\n", fill_emp[0], fill_emp[1], fill_emp[2], fill_emp[3]);
    int fill_one_a[] = {8};
    std::fill(fill_one_a, fill_one_a + 1, 3);
    std::printf("fill_one=%d\n", fill_one_a[0]);
    std::fill(zn, zn, 0);
    std::printf("fill_null=%d\n", 1);
    PredOnly fill_preds[] = {PredOnly(3), PredOnly(1), PredOnly(3)};
    PredOnly fill_value(9);
    std::fill(fill_preds, fill_preds + 3, fill_value);
    PredOnly fill_preds_empty[] = {PredOnly(4), PredOnly(5)};
    std::fill(fill_preds_empty, fill_preds_empty, PredOnly(8));
    std::printf("fill_pred=%d,%d,%d fill_pred_empty=%d,%d\n", fill_preds[0].x, fill_preds[1].x, fill_preds[2].x,
                fill_preds_empty[0].x, fill_preds_empty[1].x);

    int copy_src[] = {9, 4, 1, 4};
    int copy_dst[] = {0, 0, 0, 0};
    int* copy_end = std::copy(copy_src, copy_src + 4, copy_dst);
    std::printf("copy_all=%d,%d,%d,%d end=%d src=%d,%d,%d,%d\n", copy_dst[0], copy_dst[1], copy_dst[2], copy_dst[3],
                copy_end == copy_dst + 4 ? 1 : 0, copy_src[0], copy_src[1], copy_src[2], copy_src[3]);
    int copy_span_src[] = {9, 4, 1, 4};
    int copy_span_dst[] = {8, 8, 8, 8};
    int* copy_span_end = std::copy(copy_span_src, copy_span_src + 3, copy_span_dst);
    std::printf("copy_span=%d,%d,%d,%d end=%d\n", copy_span_dst[0], copy_span_dst[1], copy_span_dst[2],
                copy_span_dst[3], copy_span_end == copy_span_dst + 3 ? 1 : 0);
    int copy_emp_src[] = {9, 4, 1, 4};
    int copy_emp_dst[] = {8, 7, 6, 5};
    int* copy_emp_end = std::copy(copy_emp_src, copy_emp_src, copy_emp_dst);
    std::printf("copy_empty=%d,%d,%d,%d end=%d\n", copy_emp_dst[0], copy_emp_dst[1], copy_emp_dst[2], copy_emp_dst[3],
                copy_emp_end == copy_emp_dst ? 1 : 0);
    int copy_one_src[] = {8};
    int copy_one_dst[] = {0};
    int* copy_one_end = std::copy(copy_one_src, copy_one_src + 1, copy_one_dst);
    std::printf("copy_one=%d end=%d\n", copy_one_dst[0], copy_one_end == copy_one_dst + 1 ? 1 : 0);
    int* copy_null_end = std::copy(zn, zn, zn);
    std::printf("copy_null=%d\n", copy_null_end == nullptr ? 1 : 0);
    PredOnly copy_preds[] = {PredOnly(3), PredOnly(1), PredOnly(3)};
    PredOnly copy_pred_dst[] = {PredOnly(0), PredOnly(0), PredOnly(0), PredOnly(5)};
    std::copy(copy_preds, copy_preds + 3, copy_pred_dst);
    PredOnly copy_pred_empty_src[] = {PredOnly(4), PredOnly(5)};
    PredOnly copy_pred_empty_dst[] = {PredOnly(4), PredOnly(5)};
    std::copy(copy_pred_empty_src, copy_pred_empty_src, copy_pred_empty_dst);
    std::printf("copy_pred=%d,%d,%d,%d copy_pred_empty=%d,%d\n", copy_pred_dst[0].x, copy_pred_dst[1].x,
                copy_pred_dst[2].x, copy_pred_dst[3].x, copy_pred_empty_dst[0].x, copy_pred_empty_dst[1].x);
    int copy_adj[] = {1, 2, 3, 0, 0, 0};
    int* copy_adj_end = std::copy(copy_adj, copy_adj + 3, copy_adj + 3);
    std::printf("copy_adj=%d,%d,%d,%d,%d,%d end=%d\n", copy_adj[0], copy_adj[1], copy_adj[2], copy_adj[3], copy_adj[4],
                copy_adj[5], copy_adj_end == copy_adj + 6 ? 1 : 0);
    return 0;
}
