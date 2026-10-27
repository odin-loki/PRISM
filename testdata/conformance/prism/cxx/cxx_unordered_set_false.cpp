#include <unordered_set>

int cxx_unordered_set_false(int k) {
    std::unordered_set<int> s;
    s.insert(k);
    return s.count(k) == 0 ? 0 : 1;
}
