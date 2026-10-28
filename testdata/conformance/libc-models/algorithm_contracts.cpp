// Names <algorithm> so the C++ model-header conformance case sees this
// include. The scored checks are
// testdata/conformance/prism/cxx/cxx_algorithm_true.cpp and
// cxx_algorithm_false.cpp. sort, copy, fill, and for_each are not
// called: this header does not declare them. min of the extreme ints is
// not 0. An inverted clamp is a precondition, not a value this header
// returns.
#include <algorithm>

int algorithm_minmax_true(int a, int b) {
    if (std::min(-2147483647 - 1, 2147483647) == 0) return 1;
    if (std::max(-2147483647 - 1, 2147483647) == 0) return 1;
    if (a != 3 || b != 7) return 0;
    if (std::min(a, b) != a) return 1;
    if (std::max(a, b) != b) return 1;
    if (std::min(b, a) != a) return 1;
    if (std::max(b, a) != b) return 1;
    if (std::clamp(5, a, b) != 5) return 1;
    if (std::clamp(0, a, b) != a) return 1;
    if (std::clamp(9, a, b) != b) return 1;
    return 0;
}
