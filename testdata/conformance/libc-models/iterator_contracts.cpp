// Names <iterator> so the C++ model-header conformance case sees this
// include. The scored checks are
// testdata/conformance/prism/cxx/cxx_iterator_true.cpp and
// testdata/conformance/prism/cxx/cxx_iterator_false.cpp.
// An empty array compares begin and end and does not dereference them.
// distance of two pointers is the element count. It does not load the
// element. This is not a proof that the two pointers address the same array.
#include <iterator>

int iterator_array_true(int x) {
    int a[3] = {x, 0, 1};
    int* b = std::begin(a);
    int* e = std::end(a);
    if (e != b + 3) return 1;
    if (std::distance(b, e) != 3) return 1;
    if (std::distance(e, b) != -3) return 1;
    int none[0] = {};
    if (std::begin(none) != std::end(none)) return 1;
    if (std::distance(std::begin(none), std::end(none)) != 0) return 1;
    if (*b != x) return 1;
    return 0;
}
