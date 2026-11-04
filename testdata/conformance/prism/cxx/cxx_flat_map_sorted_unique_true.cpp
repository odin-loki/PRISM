// PRISM conformance task cxx/cxx_flat_map_sorted_unique_true.cpp: expected true (no-uncaught)
#include <flat_map>
#include <utility>
int cxx_flat_map_sorted_unique_true(int x) {
    std::flat_map<int, int> m(std::sorted_unique, {{1, x}, {2, 3}});
    return m.at(1) - x + m.at(2);
}
