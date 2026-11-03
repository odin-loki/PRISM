// PRISM conformance task cxx/cxx_unordered_map_false.cpp: expected false (no-uncaught)
// at() of a key that was never inserted throws std::out_of_range, uncaught
#include <unordered_map>

int cxx_unordered_map_false(int k) {
    std::unordered_map<int, int> m;
    m[0] = 1;
    return m.at(k) == 0 ? 0 : 1;
}
