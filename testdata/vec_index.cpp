#include <vector>

int vec_index_bad(int i) {
    std::vector<int> v;
    return v[i];
}

int vec_index_ok(int i) {
    std::vector<int> v;
    if (i < v.size())
        return v[i];
    return 0;
}
