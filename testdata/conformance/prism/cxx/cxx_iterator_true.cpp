// PRISM conformance task cxx/cxx_iterator_true.cpp: expected true (no-oob).
// begin is a pointer to the first element. end is one past the last.
// The empty array compares those pointers and does not dereference them.
// This is not a proof that an iterator stays in range.
#include <iterator>
int cxx_iterator_true(int x) {
    int a[3] = {x, 0, 1};
    int* b = std::begin(a);
    int* e = std::end(a);
    if (e != b + 3) return 0;
    int none[0] = {};
    if (std::begin(none) != std::end(none)) return 0;
    return *b;
}
