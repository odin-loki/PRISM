// PRISM conformance task cxx/cxx_initializer_list_false.cpp: expected false (memsafety)
// begin()[3] reads past the three-element backing array
#include <initializer_list>
int cxx_initializer_list_false(int n) {
    std::initializer_list<int> il{1, 2, 3};
    if (n < 0 || n > 3) return 0;
    return il.begin()[n];
}
