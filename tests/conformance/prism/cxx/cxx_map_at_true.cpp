// PRISM conformance task cxx/cxx_map_at_true.cpp: expected true (no-uncaught)
// std::map::at of an absent key in a noexcept function (std::terminate) (guarded)
#include <map>
#include <stdexcept>
int cxx_map_at_true(int k) noexcept {
    std::map<int, int> m;
    m[1] = 10;
    m[3] = 30;
    if (!m.contains(k & 3)) return 0;
    return m.at(k & 3);
}
