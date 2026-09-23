// PRISM conformance task cxx/cxx_structured_true.cpp: expected true (no-overflow)
#include <utility>
int cxx_structured_true(int x) {
    auto [lo, hi] = std::pair<int, int>{x & 7, 9};
    return lo * hi;
}
