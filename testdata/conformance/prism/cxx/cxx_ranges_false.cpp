// PRISM conformance task cxx/cxx_ranges_false.cpp: expected false (no-overflow)
// ssize of a size that does not fit in ptrdiff_t. A non-zero input takes
// that path. The model checks FUNC-CONTRACT and does not return the wrapped
// negative value.
#include <ranges>
#include <cstdint>
struct RangesWide {
    unsigned long long n;
    unsigned long long size() const { return n; }
};
int cxx_ranges_false(int c) {
    if (c == 0) return 0;
    RangesWide w;
    w.n = static_cast<unsigned long long>(PTRDIFF_MAX) + 1ull;
    return static_cast<int>(std::ranges::ssize(w));
}
