// Trace of std::complex addition, real/imag, and abs of values whose squares
// fit (tests/cpp/test_cxx_complex.cpp checks the zero-divisor precondition and
// the overflowing abs on the model header directly).
// `#include <complex>` is this model when its directory is first on the
// include path, and libstdc++ otherwise. The lines from a run with no
// arguments are the same either way. Division by zero is not traced:
// libstdc++ yields a NaN, and the model reports a precondition check instead.
// argv "ovf" is a finite part whose square overflows. argv "inf" and "nan"
// are non-finite parts. The model checks and does not print mag=.
#include <complex>
#include <cstdio>
#include <cstring>

// Native stand-in for the PIR intrinsic. A failing check prints and returns;
// the model then traps, so mag= is not printed. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
    }
}

int main(int argc, char** argv) {
    if (argc > 1) {
        volatile double huge = 1e300;
        double part = huge;
        if (std::strcmp(argv[1], "ovf") == 0) {
            part = huge;
        } else if (std::strcmp(argv[1], "inf") == 0) {
            part = huge * huge;
        } else if (std::strcmp(argv[1], "nan") == 0) {
            const double inf = huge * huge;
            part = inf - inf;
        } else {
            return 2;
        }
        const double m = std::abs(std::complex<double>(part, 0.0));
        std::printf("mag=%.17g\n", m);
        return 0;
    }
    const std::complex<double> z(1.5, -2.25);
    const std::complex<double> s = z + std::complex<double>(0.5, 2.25);
    const double a0 = std::abs(std::complex<double>(0.0, 0.0));
    const double a34 = std::abs(std::complex<double>(3.0, 4.0));
    std::printf("ctor %.17g %.17g\n", z.real(), z.imag());
    std::printf("add %.17g %.17g\n", s.real(), s.imag());
    std::printf("abs0 %.17g\n", a0);
    std::printf("abs34 %.17g\n", a34);
    if (z.real() != 1.5 || z.imag() != -2.25) return 1;
    if (s.real() != 2.0 || s.imag() != 0.0) return 1;
    if (a0 != 0.0 || a34 != 5.0) return 1;
    return 0;
}
