// Differential trace of <cmath> (tests/cpp/test_cxx_cmath.cpp): compiled once
// against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/cmath) first on the include path. The happy path
// prints the same lines. argv "neg" calls sqrt on a negative value. The model
// must __prism_check and must not print a value. isfinite(0) is 1, isnan(0)
// is 0, isinf(0) is 0, and signbit of a negative value is 1. Those classify;
// they are not a proof of a numeric identity.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned value. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "neg") == 0) {
        volatile double n = -1.0;
        double v = std::sqrt(n);
        std::printf("value=%g\n", v);
        return 0;
    }
    volatile double z = 0.0;
    volatile double four = 4.0;
    volatile double neg2 = -2.0;
    volatile double neg25 = -2.5;
    // Integer tokens, so a decimal comma in the C locale cannot change the line.
    // fabs2 is 2 only when fabs(-2) is the magnitude 2, not 0 and not a unit.
    std::printf("sqrt0=%d\n", std::sqrt(z) == 0.0 ? 0 : -1);
    std::printf("sqrt4=%d\n", std::sqrt(four) == 2.0 ? 2 : -1);
    std::printf("fabs2=%d\n", std::fabs(neg2) == 2.0 ? 2 : -1);
    std::printf("fabs25=%d\n", std::fabs(neg25) == 2.5 ? 1 : 0);
    std::printf("abs25=%d\n", std::abs(neg25) == 2.5 ? 1 : 0);
    volatile float zf = 0.0f;
    volatile long double zl = 0.0L;
    volatile float nf = -2.0f;
    volatile long double nl = -2.0L;
    std::printf("fin0=%d\n", std::isfinite(z) ? 1 : 0);
    std::printf("fin0f=%d\n", std::isfinite(zf) ? 1 : 0);
    std::printf("fin0l=%d\n", std::isfinite(zl) ? 1 : 0);
    std::printf("nan0=%d\n", std::isnan(z) ? 1 : 0);
    std::printf("nan0f=%d\n", std::isnan(zf) ? 1 : 0);
    std::printf("nan0l=%d\n", std::isnan(zl) ? 1 : 0);
    std::printf("inf0=%d\n", std::isinf(z) ? 1 : 0);
    std::printf("inf0f=%d\n", std::isinf(zf) ? 1 : 0);
    std::printf("inf0l=%d\n", std::isinf(zl) ? 1 : 0);
    std::printf("sgn2=%d\n", std::signbit(neg2) ? 1 : 0);
    std::printf("sgnf=%d\n", std::signbit(nf) ? 1 : 0);
    std::printf("sgnl=%d\n", std::signbit(nl) ? 1 : 0);
    return 0;
}
