// Trace of std::complex addition and real/imag (tests/cpp/test_cxx_complex.cpp
// checks the zero-divisor precondition on the model header directly).
// `#include <complex>` is this model when its directory is first on the
// include path, and libstdc++ otherwise. These lines are the same either way.
// Division by zero is not traced: libstdc++ yields a NaN, and the model
// reports a precondition check instead.
#include <complex>
#include <cstdio>

int main() {
    const std::complex<double> z(1.5, -2.25);
    const std::complex<double> s = z + std::complex<double>(0.5, 2.25);
    std::printf("ctor %.17g %.17g\n", z.real(), z.imag());
    std::printf("add %.17g %.17g\n", s.real(), s.imag());
    if (z.real() != 1.5 || z.imag() != -2.25) return 1;
    if (s.real() != 2.0 || s.imag() != 0.0) return 1;
    return 0;
}
