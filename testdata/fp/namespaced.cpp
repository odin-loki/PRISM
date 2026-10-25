// Correct C++: namespaces, templates, trailing return types, operators.
#include <string>

namespace util {
namespace detail {

template <typename T>
T clamp_to(T v, T lo, T hi) {
    return v < lo ? lo : (hi < v ? hi : v);
}

}  // namespace detail

auto twice(int x) -> int {
    return detail::clamp_to(x, -1000, 1000) * 2;
}

struct Point {
    int x = 0;
    int y = 0;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

std::string label(const Point& p) {
    return std::to_string(p.x) + "," + std::to_string(p.y);
}

}  // namespace util
