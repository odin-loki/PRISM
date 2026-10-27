// PRISM conformance task cxx/cxx_valarray_false.cpp: expected false (no-oob)
// operator[] on an empty valarray (x == 0 leaves it empty)
#include <valarray>
int cxx_valarray_false(int x) {
    std::valarray<int> v;
    if (x) return x;
    return v[0];
}
