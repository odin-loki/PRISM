// Differential trace of <cmath> (tests/cpp/test_cxx_cmath.cpp): compiled once
// against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/cmath) first on the include path. The happy path
// prints the same lines. argv "neg" calls sqrt on a negative value. The model
// must __prism_check and must not print a value.
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
    return 0;
}
