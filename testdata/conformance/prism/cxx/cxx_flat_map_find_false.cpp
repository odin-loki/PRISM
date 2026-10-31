// PRISM conformance task cxx/cxx_flat_map_find_false.cpp: expected false (no-oob)
#include <flat_map>
int cxx_flat_map_find_false(int s) {
    std::flat_map<int, int> m;
    m[1] = 4; m[5] = 9;
    int b[5] = {};
    auto it = m.find(s);
    return it == m.end() ? 0 : b[it->second];
}
