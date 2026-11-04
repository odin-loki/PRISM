// PRISM conformance task cxx/cxx_ranges_views_true.cpp: expected true (no-overflow)
// views::take over an array
#include <array>
#include <ranges>
int cxx_ranges_views_true(int k) {
    std::array<int, 4> a{1, 2, 3, 4};
    int s = 0;
    for (int v : a | std::views::take(3)) s += v;
    return (k >= 0 && k < 100) ? s + k : s;
}
