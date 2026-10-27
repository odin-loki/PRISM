// PRISM conformance task cxx/cxx_ranges_true.cpp: expected true (no-overflow)
// size of an array is N. size of an lvalue with a const size() member is
// that size(). ssize of a value that fits in ptrdiff_t is that value.
#include <ranges>
struct RangesCount {
    int n;
    int size() const { return n; }
};
int cxx_ranges_true(int n) {
    int a[4] = {};
    if (std::ranges::size(a) != 4) return 1;
    if (std::ranges::ssize(a) != 4) return 1;
    if (n < 0) n = 0;
    RangesCount c;
    c.n = n;
    if (std::ranges::size(c) != n) return 1;
    if (std::ranges::ssize(c) != n) return 1;
    return 0;
}
