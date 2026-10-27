// Names <ranges> so the C++ model-header conformance case sees this include.
// The scored tasks are testdata/conformance/prism/cxx/cxx_ranges_true.cpp
// (array N, and a const size() member, including ssize of a value that
// fits) and cxx_ranges_false.cpp (ssize of a size that does not fit in
// ptrdiff_t). begin, end, sort, find, and views are not called.
#include <ranges>

struct RangesCount {
    int n;
    int size() const { return n; }
};

int ranges_array_true(int) {
    int a[4] = {};
    if (std::ranges::size(a) != 4) return 1;
    if (std::ranges::ssize(a) != 4) return 1;
    return 0;
}

int ranges_member_true(int n) {
    if (n < 0) n = 0;
    RangesCount c;
    c.n = n;
    if (std::ranges::size(c) != n) return 1;
    if (std::ranges::ssize(c) != n) return 1;
    return 0;
}
