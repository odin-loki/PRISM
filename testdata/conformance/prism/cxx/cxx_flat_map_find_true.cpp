// PRISM conformance task cxx/cxx_flat_map_find_true.cpp: expected true (no-oob)
#include <flat_map>
int cxx_flat_map_find_true(int s) {
    std::flat_map<int, int> m;
    m[1] = 4; m[5] = 9;
    int b[10] = {};
    auto it = m.find(s);
    return it == m.end() ? 0 : b[it->second];  // mapped values 4 and 9 index b[10]
}
