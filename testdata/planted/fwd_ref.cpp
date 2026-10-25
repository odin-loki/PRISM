#include <vector>
#include <utility>

void fwd_bad(int &&x) {
    std::vector<int> v;
    v.push_back(x);
}

void fwd_ok(int &&x) {
    std::vector<int> v;
    v.push_back(std::move(x));
}
