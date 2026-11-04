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
int destroyn_bad(int *p) {
    std::destroy_n(p, 4);
    return p[0];
}
int destroyn_ok(int *p) {
    std::destroy_n(p, 4);
    return 0;
}
