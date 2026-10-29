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
int killdep_bad(int *p, int x) { return p[std::kill_dependency(x)]; }
int killdep_ok(int *p, int x) {
    auto y = std::kill_dependency(x);
    if (y < 0 || y > 8) return 0;
    return p[y];
}
