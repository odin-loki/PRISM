// std::complex model (src/prism/pir/models/cxx/complex). The pir stage puts
// the model directory first on the include path, so <complex> below is the
// model. The false function divides by a zero complex.
#include <complex>

int complex_add_true(int c) {
    std::complex<double> a(1.0, 2.0);
    std::complex<double> b(3.0, static_cast<double>(c & 7));
    std::complex<double> s = a + b;
    if (s.real() != 4.0) return 0;
    return s.imag() == 2.0 + static_cast<double>(c & 7) ? 1 : 0;
}

int complex_div0_false(int c) {
    std::complex<double> a(static_cast<double>(c), 1.0);
    std::complex<double> z = a / std::complex<double>(0, 0);
    return z.imag() == 1.0 ? 1 : 0;
}
