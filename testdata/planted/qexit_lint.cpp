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
void qexit_bad(void) { std::quick_exit(1); }
void qexit_ok(void) {
    std::at_quick_exit([]{});
    std::quick_exit(0);
}
