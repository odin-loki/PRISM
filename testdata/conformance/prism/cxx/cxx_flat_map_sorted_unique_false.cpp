// PRISM conformance task cxx/cxx_flat_map_sorted_unique_false.cpp: expected false (no-uncaught)
#include <flat_map>
#include <utility>
int cxx_flat_map_sorted_unique_false(int x) noexcept {
    std::flat_map<int, int> m(std::sorted_unique, {{1, x}, {2, 3}});
    return m.at(1) - x + m.at(3);  // key 3 is absent: out_of_range
}
