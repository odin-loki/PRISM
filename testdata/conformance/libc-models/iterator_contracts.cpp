// Names <iterator> so the C++ model-header conformance case sees this
// include. The scored check is
// testdata/conformance/prism/cxx/cxx_iterator_true.cpp.
// There is no false twin: this header does not plant a range check.
// An empty array compares begin and end and does not dereference them.
#include <iterator>

int iterator_array_true(int x) {
    int a[3] = {x, 0, 1};
    int* b = std::begin(a);
    int* e = std::end(a);
    if (e != b + 3) return 1;
    int none[0] = {};
    if (std::begin(none) != std::end(none)) return 1;
    if (*b != x) return 1;
    return 0;
}
