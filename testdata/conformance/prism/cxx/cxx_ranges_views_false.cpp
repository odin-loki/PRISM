// PRISM conformance task cxx/cxx_ranges_views_false.cpp: expected false (no-overflow)
// views::drop sum overflows on the last element
#include <array>
#include <ranges>
int cxx_ranges_views_false(int k) {
    std::array<int, 4> a{1, 2, 3, k};
    int s = 0;
    for (int v : a | std::views::drop(2)) s += v;
    return s;
}
