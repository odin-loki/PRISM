// PRISM conformance task cxx/cxx_flat_map_true.cpp: expected true (no-uncaught)
// insert one key with operator[] and read it with at()
#include <flat_map>
int cxx_flat_map_true(int k) noexcept {
    std::flat_map<int, int> m;
    m[k] = 1;
    return m.at(k);
}
