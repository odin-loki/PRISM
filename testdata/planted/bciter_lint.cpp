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
int bciter_bad(int *p) {
    std::basic_const_iterator<int*> it(p);
    return it[9];
}
int bciter_ok(int *p) {
    std::basic_const_iterator<int*> it(p);
    return it[0];
}
