// PRISM conformance task cxx/cxx_iterator_true.cpp: expected true (no-oob).
// begin is a pointer to the first element. end is one past the last.
// distance of those pointers is the element count. It does not load the
// element. The empty array compares those pointers and does not dereference
// them. This is not a proof that an iterator stays in range, and it is not
// a proof that the two pointers address the same array.
#include <iterator>
int cxx_iterator_true(int x) {
    int a[3] = {x, 0, 1};
    int* b = std::begin(a);
    int* e = std::end(a);
    if (e != b + 3) return 0;
    if (std::distance(b, e) != 3) return 0;
    if (std::distance(e, b) != -3) return 0;
    int none[0] = {};
    if (std::begin(none) != std::end(none)) return 0;
    if (std::distance(std::begin(none), std::end(none)) != 0) return 0;
    return *b;
}
