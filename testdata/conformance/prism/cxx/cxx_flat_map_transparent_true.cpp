// PRISM conformance task cxx/cxx_flat_map_transparent_true.cpp: expected true (no-uncaught)
#include <flat_map>
#include <functional>
int cxx_flat_map_transparent_true(short x) {
    std::flat_map<long, int, std::less<>> m;
    m[7] = x;
    return m.contains(7) ? m.at(7) : 0;
}
