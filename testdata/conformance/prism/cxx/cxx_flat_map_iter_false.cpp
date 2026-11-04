// PRISM conformance task cxx/cxx_flat_map_iter_false.cpp: expected false (no-oob)
// iteration is sorted; b[(s & 1) + 2] reaches b[3] past b[3]
#include <flat_map>
int cxx_flat_map_iter_false(int s) {
    std::flat_map<int, int> m;
    m[3] = 30; m[1] = 10; m[2] = 20;
    int b[3], i = 0, prev = 0;
    for (auto [k, v] : m) { if (k <= prev) return 0; prev = k; b[i++] = v; }
    return b[(s & 1) + 2];
}
