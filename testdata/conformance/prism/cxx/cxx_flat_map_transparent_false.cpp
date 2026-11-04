// PRISM conformance task cxx/cxx_flat_map_transparent_false.cpp: expected false (no-uncaught)
#include <flat_map>
#include <functional>
int cxx_flat_map_transparent_false(short x) noexcept {
    std::flat_map<long, int, std::less<>> m;
    m[7] = x;
    return m.at(8);  // heterogeneous lookup of an absent key throws
}
