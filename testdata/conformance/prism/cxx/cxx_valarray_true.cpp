// PRISM conformance task cxx/cxx_valarray_true.cpp: expected true (no-oob)
// store one value and read it back at [0]
#include <valarray>
int cxx_valarray_true(int x) {
    std::valarray<int> v;
    v.resize(1, x);
    return v[0];
}
