// PRISM conformance task cxx/cxx_flat_map_false.cpp: expected false (no-uncaught)
// at() on an empty flat_map; the throw leaves this noexcept function
#include <flat_map>
int cxx_flat_map_false(int k) noexcept {
    std::flat_map<int, int> m;
    return m.at(k);
}
