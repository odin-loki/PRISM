// PRISM conformance task cxx/cxx_ranges_true.cpp: expected true (no-overflow)
// size of an array is N. size of an lvalue with a const size() member is
// that size(). ssize of a value that fits in ptrdiff_t is that value.
// empty of T(&)[N] is true only when N == 0, and it does not load an
// element. empty of an lvalue with a const empty() member is that empty().
#include <ranges>
struct RangesCount {
    int n;
    int size() const { return n; }
};
struct RangesFlag {
    bool e;
    bool empty() const { return e; }
};
int cxx_ranges_true(int n) {
    int a[4] = {};
    if (std::ranges::size(a) != 4) return 1;
    if (std::ranges::ssize(a) != 4) return 1;
    if (std::ranges::empty(a)) return 1;
    int none[0] = {};
    if (!std::ranges::empty(none)) return 1;
    if (n < 0) n = 0;
    RangesCount c;
    c.n = n;
    if (std::ranges::size(c) != n) return 1;
    if (std::ranges::ssize(c) != n) return 1;
    RangesFlag f;
    f.e = false;
    if (std::ranges::empty(f)) return 1;
    f.e = true;
    if (!std::ranges::empty(f)) return 1;
    return 0;
}
