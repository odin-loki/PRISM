#include <unordered_map>

int cxx_unordered_map_true(int k) {
    std::unordered_map<int, int> m;
    m[k] = 1;
    return m.at(k) == 1 ? 0 : 1;
}
