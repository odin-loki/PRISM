// PRISM conformance task cxx/cxx_algorithm_true.cpp: expected true (no-overflow)
// min, max, and clamp of a valid range return one of the arguments.
// min of the extreme ints is that extreme, not 0.
#include <algorithm>
int cxx_algorithm_true(int a, int b) {
    if (std::min(-2147483647 - 1, 2147483647) == 0) return 1;
    if (std::max(-2147483647 - 1, 2147483647) == 0) return 1;
    if (std::clamp(0, -2147483647 - 1, 2147483647) != 0) return 1;
    if (a != 3 || b != 7) return 0;
    if (std::min(a, b) != 3) return 1;
    if (std::max(a, b) != 7) return 1;
    if (std::min(b, a) != 3) return 1;
    if (std::max(b, a) != 7) return 1;
    if (std::clamp(5, a, b) != 5) return 1;
    if (std::clamp(0, a, b) != 3) return 1;
    if (std::clamp(9, a, b) != 7) return 1;
    return 0;
}
