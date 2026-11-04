// PRISM conformance task cxx/cxx_flat_map_extract_true.cpp: expected true (no-uncaught)
#include <flat_map>
#include <utility>
int cxx_flat_map_extract_true(int x) {
    std::flat_map<int, int> m;
    m[1] = x;
    auto c = std::move(m).extract();
    m.replace(std::move(c.keys), std::move(c.values));
    return m.at(1);  // key 1 survives extract/replace
}
