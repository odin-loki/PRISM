// PRISM conformance task cxx/cxx_structured_false.cpp: expected false (no-overflow)
#include <utility>
int cxx_structured_false(int x) {
    auto [lo, hi] = std::pair<int, int>{x, 9};
    return lo * hi;
}
