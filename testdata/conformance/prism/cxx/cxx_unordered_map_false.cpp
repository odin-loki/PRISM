#include <unordered_map>

int cxx_unordered_map_false(int k) {
    std::unordered_map<int, int> m;
    m[k] = 1;
    return m.at(k) == 0 ? 0 : 1;
}
