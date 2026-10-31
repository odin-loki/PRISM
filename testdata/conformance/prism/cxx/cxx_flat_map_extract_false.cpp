// PRISM conformance task cxx/cxx_flat_map_extract_false.cpp: expected false (no-uncaught)
#include <flat_map>
#include <utility>
int cxx_flat_map_extract_false(int x) {
    std::flat_map<int, int> m;
    m[1] = x;
    auto c = std::move(m).extract();
    m.replace(std::move(c.keys), std::move(c.values));
    return m.at(x & 1);  // key 0 is absent
}
