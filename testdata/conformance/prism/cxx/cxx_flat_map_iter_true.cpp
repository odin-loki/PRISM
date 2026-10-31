// PRISM conformance task cxx/cxx_flat_map_iter_true.cpp: expected true (no-oob)
#include <flat_map>
int cxx_flat_map_iter_true(int s) {
    std::flat_map<int, int> m;
    m[3] = 30; m[1] = 10; m[2] = 20;
    int b[3], i = 0, prev = 0;
    for (auto [k, v] : m) { if (k <= prev) return 1 / 0; prev = k; b[i++] = v; }
    return b[s & 1];
}
