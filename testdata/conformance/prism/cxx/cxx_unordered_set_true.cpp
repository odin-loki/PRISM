#include <unordered_set>

int cxx_unordered_set_true(int k) {
    std::unordered_set<int> s;
    s.insert(k);
    return s.count(k) == 1 ? 0 : 1;
}
