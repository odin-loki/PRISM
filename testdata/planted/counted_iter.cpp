#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>
int counted_iter_bad(int *p, int n) {
    std::counted_iterator it(p, n);
    return *it;
}

int counted_iter_ok(int *p, int n) {
    if (n <= 0)
        return 0;
    std::counted_iterator it(p, n);
    return *it;
}
