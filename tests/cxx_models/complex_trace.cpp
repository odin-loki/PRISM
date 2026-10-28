// Trace of std::complex addition, real/imag, abs, and norm of values whose
// squares fit (tests/cpp/test_cxx_complex.cpp checks the zero-divisor
// precondition and the overflowing abs and norm on the model header directly).
// `#include <complex>` is this model when its directory is first on the
// include path, and libstdc++ otherwise. The lines from a run with no
// arguments are the same either way. operator!= is the negation of
// operator==. Equal values compare not-equal as false. A NaN part compares
// not-equal to itself because operator== is false; the result is not forced
// to false or to 0. Division by zero is not traced: libstdc++ yields a NaN,
// and the model reports a precondition check instead.
// argv "ovf" is a finite part whose square overflows abs. argv "inf" and
// "nan" are non-finite parts for abs. The model checks and does not print
// mag=. argv "novf", "ninf", and "nnan" are the same inputs for norm. The
// model checks and does not print nrm=.
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>

// Native stand-in for the PIR intrinsic. A failing check prints and returns;
// the model then traps, so mag= and nrm= are not printed. A passing check
// returns.
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
        const bool norm_mode = std::strcmp(argv[1], "novf") == 0 || std::strcmp(argv[1], "ninf") == 0 ||
                               std::strcmp(argv[1], "nnan") == 0;
        const char* which = argv[1];
        if (std::strcmp(which, "novf") == 0) which = "ovf";
        if (std::strcmp(argv[1], "ninf") == 0) which = "inf";
        if (std::strcmp(argv[1], "nnan") == 0) which = "nan";
        if (std::strcmp(which, "ovf") == 0) {
            part = huge;
        } else if (std::strcmp(which, "inf") == 0) {
            part = huge * huge;
        } else if (std::strcmp(which, "nan") == 0) {
            const double inf = huge * huge;
            part = inf - inf;
        } else {
            return 2;
        }
        if (norm_mode) {
            const double n = std::norm(std::complex<double>(part, 0.0));
            std::printf("nrm=%.17g\n", n);
            return 0;
        }
        const double m = std::abs(std::complex<double>(part, 0.0));
        std::printf("mag=%.17g\n", m);
        return 0;
    }
    const std::complex<double> z(1.5, -2.25);
    const std::complex<double> s = z + std::complex<double>(0.5, 2.25);
    const double a0 = std::abs(std::complex<double>(0.0, 0.0));
    const double a34 = std::abs(std::complex<double>(3.0, 4.0));
    const double n0 = std::norm(std::complex<double>(0.0, 0.0));
    const double n34 = std::norm(std::complex<double>(3.0, 4.0));
    const std::complex<double> same(1.5, -2.25);
    const std::complex<double> re(9.0, -2.25);
    const std::complex<double> im(1.5, 4.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::complex<double> nr(nan, 1.0);
    const std::complex<double> ni(1.0, nan);
    const std::complex<double> nb(nan, nan);
    const int ne_same = static_cast<int>(z != same);
    const int ne_re = static_cast<int>(z != re);
    const int ne_im = static_cast<int>(z != im);
    const int ne_self = static_cast<int>(z != z);
    const int ne_neg0 = static_cast<int>(std::complex<double>(-0.0, -0.0) != std::complex<double>(0.0, 0.0));
    const int nan_re = static_cast<int>(nr != nr);
    const int nan_im = static_cast<int>(ni != ni);
    const int nan_both = static_cast<int>(nb != nb);
    std::printf("ctor %.17g %.17g\n", z.real(), z.imag());
    std::printf("add %.17g %.17g\n", s.real(), s.imag());
    std::printf("abs0 %.17g\n", a0);
    std::printf("abs34 %.17g\n", a34);
    std::printf("norm0 %.17g\n", n0);
    std::printf("norm34 %.17g\n", n34);
    std::printf("ne same=%d re=%d im=%d self=%d neg0=%d\n", ne_same, ne_re, ne_im, ne_self, ne_neg0);
    std::printf("nenan re=%d im=%d both=%d\n", nan_re, nan_im, nan_both);
    if (z.real() != 1.5 || z.imag() != -2.25) return 1;
    if (s.real() != 2.0 || s.imag() != 0.0) return 1;
    if (a0 != 0.0 || a34 != 5.0) return 1;
    if (n0 != 0.0 || n34 != 25.0) return 1;
    if (ne_same != 0 || ne_re != 1 || ne_im != 1 || ne_self != 0 || ne_neg0 != 0) return 1;
    if (nan_re != 1 || nan_im != 1 || nan_both != 1) return 1;
    return 0;
}
