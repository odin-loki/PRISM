// PRISM conformance task cxx/cxx_complex_false.cpp: expected false (memsafety)
// division by a zero complex (both parts zero)
#include <complex>
int cxx_complex_false(int c) {
    std::complex<double> a(1.0, static_cast<double>(c));
    std::complex<double> z = a / std::complex<double>(0, 0);
    return z.real() == z.real() ? 0 : 1;
}
