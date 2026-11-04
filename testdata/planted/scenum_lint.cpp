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
enum class E { A };

int scenum_bad(int *p) {
    using ok = std::is_scoped_enum<E>;
    return p[9];
}
int scenum_ok(int *p) {
    using ok = std::is_scoped_enum<E>;
    return p[0];
}
