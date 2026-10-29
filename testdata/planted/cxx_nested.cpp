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
int nested_unenc_bad(int n) {
    std::nested_exception x;
    return n;
}

int nested_unenc_ok(int n) {
    return n;
}
