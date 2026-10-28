// PRISM conformance task cxx/cxx_make_unique_true.cpp: expected true (memsafety)
// make_unique<int> owns one int. make_unique<int[]>(0) does not invent an
// element. make_unique<int[]>(2) value-initializes two ints; the unique_ptr
// frees them with array delete.
#include <memory>
int cxx_make_unique_true(int c) {
    auto p = std::make_unique<int>(c);
    auto z = std::make_unique<int[]>(0);
    (void)z;
    auto a = std::make_unique<int[]>(2);
    a[0] = *p;
    a[1] = 1;
    return a[0] + a[1];
}
