// PRISM conformance task cxx/cxx_complex_true.cpp: expected true (memsafety)
// addition, then real() and imag() only
#include <complex>
int cxx_complex_true(int c) {
    std::complex<double> a(1.0, 2.0);
    std::complex<double> b(3.0, static_cast<double>(c & 7));
    std::complex<double> s = a + b;
    if (s.real() != 4.0) return 0;
    return s.imag() == 2.0 + static_cast<double>(c & 7) ? 1 : 0;
}
