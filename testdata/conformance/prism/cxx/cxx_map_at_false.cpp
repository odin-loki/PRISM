// PRISM conformance task cxx/cxx_map_at_false.cpp: expected false (no-uncaught)
// std::map::at of an absent key in a noexcept function (std::terminate)
#include <map>
#include <stdexcept>
int cxx_map_at_false(int k) noexcept {
    std::map<int, int> m;
    m[1] = 10;
    m[3] = 30;
    return m.at(k & 3);  // k = 0 or 2: out_of_range escapes noexcept
}
