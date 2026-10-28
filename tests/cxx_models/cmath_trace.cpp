// Differential trace of <cmath> (tests/cpp/test_cxx_cmath.cpp): compiled once
// against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/cmath) first on the include path. The happy path
// prints the same lines. argv "neg" calls sqrt on a negative value. The model
// must __prism_check and must not print a value. isfinite(0) is 1, isnan(0)
// is 0, isinf(0) is 0, and signbit of a negative value is 1. Those classify;
// they are not a proof of a numeric identity. copysign(0, negative) prints
// as a negative zero. fmin/fmax of one NaN print the other argument. Two
// NaNs print as a NaN, not as zero. Those three are not PROVED and not CLEAN.
// floor(1.5) is 1 and floor(-1.5) is -2. ceil(1.5) is 2 and ceil(-1.5) is
// -1. trunc(1.5) is 1 and trunc(-1.5) is -1. A NaN stays a NaN, not zero.
// An infinity stays that infinity. A negative zero stays a negative zero.
// floor, ceil, and trunc are not PROVED and not CLEAN.
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

    volatile double pos = 2.5;
    volatile double negv = -2.5;
    volatile double one = 1.0;
    volatile double neg1 = -1.0;
    volatile double three = 3.0;
    volatile double d0 = 0.0;
    std::printf("cp25=%d\n", std::copysign(negv, one) == 2.5 ? 1 : 0);
    std::printf("cpn=%d\n", std::copysign(pos, neg1) == -2.5 ? 1 : 0);
    volatile double cp0 = std::copysign(d0, neg1);
    std::printf("cp0=%d\n", (cp0 == 0.0 && std::signbit(cp0)) ? 1 : 0);
    volatile double nzero = -d0;
    volatile double cp0p = std::copysign(nzero, one);
    std::printf("cp0p=%d\n", (cp0p == 0.0 && !std::signbit(cp0p)) ? 1 : 0);
    std::printf("cpmag=%d\n", std::fabs(std::copysign(negv, neg1)) == 2.5 ? 1 : 0);

    volatile float f0 = 0.0f;
    volatile float fn = -1.0f;
    volatile float fpos = 2.5f;
    volatile float fcp0 = std::copysign(f0, fn);
    std::printf("cp0f=%d\n", (fcp0 == 0.0f && std::signbit(fcp0)) ? 1 : 0);
    std::printf("cpnf=%d\n", std::copysign(fpos, fn) == -2.5f ? 1 : 0);

    volatile long double l0 = 0.0L;
    volatile long double ln1 = -1.0L;
    volatile long double lpos = 2.5L;
    volatile long double lcp0 = std::copysign(l0, ln1);
    std::printf("cp0l=%d\n", (lcp0 == 0.0L && std::signbit(lcp0)) ? 1 : 0);
    std::printf("cpnl=%d\n", std::copysign(lpos, ln1) == -2.5L ? 1 : 0);

    std::printf("fmin13=%d\n", std::fmin(three, one) == 1.0 ? 1 : 0);
    std::printf("fmax13=%d\n", std::fmax(negv, one) == 1.0 ? 1 : 0);
    std::printf("fminn=%d\n", std::fmin(negv, one) == -2.5 ? 1 : 0);
    volatile double nand = __builtin_nan("");
    volatile double fmin_nan = std::fmin(nand, three);
    std::printf("fminnan=%d\n", fmin_nan == 3.0 ? 1 : 0);
    volatile double fmin_nan2 = std::fmin(three, nand);
    std::printf("fminnan2=%d\n", fmin_nan2 == 3.0 ? 1 : 0);
    volatile double fmax_nan = std::fmax(nand, one);
    std::printf("fmaxnan=%d\n", fmax_nan == 1.0 ? 1 : 0);
    volatile double fmax_nan2 = std::fmax(one, nand);
    std::printf("fmaxnan2=%d\n", fmax_nan2 == 1.0 ? 1 : 0);
    volatile double bothmin = std::fmin(nand, __builtin_nan("1"));
    std::printf("fminnn=%d\n", std::isnan(bothmin) ? 1 : 0);
    volatile double bothmax = std::fmax(__builtin_nan("1"), nand);
    std::printf("fmaxnn=%d\n", std::isnan(bothmax) ? 1 : 0);

    volatile float nanf = __builtin_nanf("");
    volatile float fone = 1.0f;
    volatile float fthree = 3.0f;
    std::printf("fminf=%d\n", std::fmin(fthree, fone) == 1.0f ? 1 : 0);
    std::printf("fmaxf=%d\n", std::fmax(fone, fthree) == 3.0f ? 1 : 0);
    std::printf("fminnf=%d\n", std::fmin(nanf, fone) == 1.0f ? 1 : 0);
    std::printf("fmaxnnf=%d\n", std::isnan(std::fmax(nanf, __builtin_nanf("1"))) ? 1 : 0);

    volatile long double nanl = __builtin_nanl("");
    volatile long double lone = 1.0L;
    volatile long double lthree = 3.0L;
    std::printf("fminl=%d\n", std::fmin(lone, lthree) == 1.0L ? 1 : 0);
    std::printf("fmaxl=%d\n", std::fmax(lthree, lone) == 3.0L ? 1 : 0);
    std::printf("fminnl=%d\n", std::fmin(lone, nanl) == 1.0L ? 1 : 0);
    std::printf("fmaxnnl=%d\n", std::isnan(std::fmax(nanl, __builtin_nanl("1"))) ? 1 : 0);

    volatile double fl_a = 1.5;
    volatile double fl_n = -1.5;
    volatile double fl_two = 2.0;
    volatile double fl_ntwo = -2.0;
    std::printf("fl15=%d\n", std::floor(fl_a) == 1.0 ? 1 : 0);
    std::printf("fln15=%d\n", std::floor(fl_n) == -2.0 ? 1 : 0);
    std::printf("ce15=%d\n", std::ceil(fl_a) == 2.0 ? 1 : 0);
    std::printf("cen15=%d\n", std::ceil(fl_n) == -1.0 ? 1 : 0);
    std::printf("tr15=%d\n", std::trunc(fl_a) == 1.0 ? 1 : 0);
    std::printf("trn15=%d\n", std::trunc(fl_n) == -1.0 ? 1 : 0);
    std::printf("fl2=%d\n", std::floor(fl_two) == 2.0 ? 1 : 0);
    std::printf("fln2=%d\n", std::floor(fl_ntwo) == -2.0 ? 1 : 0);
    std::printf("ce2=%d\n", std::ceil(fl_two) == 2.0 ? 1 : 0);
    std::printf("cen2=%d\n", std::ceil(fl_ntwo) == -2.0 ? 1 : 0);
    std::printf("tr2=%d\n", std::trunc(fl_two) == 2.0 ? 1 : 0);
    std::printf("trn2=%d\n", std::trunc(fl_ntwo) == -2.0 ? 1 : 0);
    std::printf("flnan=%d\n", std::isnan(std::floor(nand)) ? 1 : 0);
    std::printf("cenan=%d\n", std::isnan(std::ceil(nand)) ? 1 : 0);
    std::printf("trnan=%d\n", std::isnan(std::trunc(nand)) ? 1 : 0);
    volatile double pinf = __builtin_huge_val();
    volatile double ninf = -__builtin_huge_val();
    std::printf("flpinf=%d\n", std::floor(pinf) == pinf ? 1 : 0);
    std::printf("flninf=%d\n", std::floor(ninf) == ninf ? 1 : 0);
    std::printf("cepinf=%d\n", std::ceil(pinf) == pinf ? 1 : 0);
    std::printf("ceninf=%d\n", std::ceil(ninf) == ninf ? 1 : 0);
    std::printf("trpinf=%d\n", std::trunc(pinf) == pinf ? 1 : 0);
    std::printf("trninf=%d\n", std::trunc(ninf) == ninf ? 1 : 0);
    volatile double mzero = -0.0;
    volatile double pzero = 0.0;
    volatile double flmz = std::floor(mzero);
    volatile double cemz = std::ceil(mzero);
    volatile double trmz = std::trunc(mzero);
    volatile double flpz = std::floor(pzero);
    std::printf("flmz=%d\n", (flmz == 0.0 && std::signbit(flmz)) ? 1 : 0);
    std::printf("cemz=%d\n", (cemz == 0.0 && std::signbit(cemz)) ? 1 : 0);
    std::printf("trmz=%d\n", (trmz == 0.0 && std::signbit(trmz)) ? 1 : 0);
    std::printf("flpz=%d\n", (flpz == 0.0 && !std::signbit(flpz)) ? 1 : 0);

    volatile float fa = 1.5f;
    volatile float fn15 = -1.5f;
    std::printf("fl15f=%d\n", std::floor(fa) == 1.0f ? 1 : 0);
    std::printf("fln15f=%d\n", std::floor(fn15) == -2.0f ? 1 : 0);
    std::printf("ce15f=%d\n", std::ceil(fa) == 2.0f ? 1 : 0);
    std::printf("cen15f=%d\n", std::ceil(fn15) == -1.0f ? 1 : 0);
    std::printf("tr15f=%d\n", std::trunc(fa) == 1.0f ? 1 : 0);
    std::printf("trn15f=%d\n", std::trunc(fn15) == -1.0f ? 1 : 0);
    std::printf("flnanf=%d\n", std::isnan(std::floor(nanf)) ? 1 : 0);
    std::printf("cenanf=%d\n", std::isnan(std::ceil(nanf)) ? 1 : 0);
    std::printf("trnanf=%d\n", std::isnan(std::trunc(nanf)) ? 1 : 0);
    volatile float pinff = __builtin_huge_valf();
    volatile float ninff = -__builtin_huge_valf();
    std::printf("flpinff=%d\n", std::floor(pinff) == pinff ? 1 : 0);
    std::printf("flninff=%d\n", std::floor(ninff) == ninff ? 1 : 0);
    std::printf("cepinff=%d\n", std::ceil(pinff) == pinff ? 1 : 0);
    std::printf("ceninff=%d\n", std::ceil(ninff) == ninff ? 1 : 0);
    std::printf("trpinff=%d\n", std::trunc(pinff) == pinff ? 1 : 0);
    std::printf("trninff=%d\n", std::trunc(ninff) == ninff ? 1 : 0);
    volatile float fmzero = -0.0f;
    volatile float flmzf = std::floor(fmzero);
    volatile float cemzf = std::ceil(fmzero);
    volatile float trmzf = std::trunc(fmzero);
    std::printf("flmzf=%d\n", (flmzf == 0.0f && std::signbit(flmzf)) ? 1 : 0);
    std::printf("cemzf=%d\n", (cemzf == 0.0f && std::signbit(cemzf)) ? 1 : 0);
    std::printf("trmzf=%d\n", (trmzf == 0.0f && std::signbit(trmzf)) ? 1 : 0);

    volatile long double la = 1.5L;
    volatile long double ln15 = -1.5L;
    std::printf("fl15l=%d\n", std::floor(la) == 1.0L ? 1 : 0);
    std::printf("fln15l=%d\n", std::floor(ln15) == -2.0L ? 1 : 0);
    std::printf("ce15l=%d\n", std::ceil(la) == 2.0L ? 1 : 0);
    std::printf("cen15l=%d\n", std::ceil(ln15) == -1.0L ? 1 : 0);
    std::printf("tr15l=%d\n", std::trunc(la) == 1.0L ? 1 : 0);
    std::printf("trn15l=%d\n", std::trunc(ln15) == -1.0L ? 1 : 0);
    std::printf("flnanl=%d\n", std::isnan(std::floor(nanl)) ? 1 : 0);
    std::printf("cenanl=%d\n", std::isnan(std::ceil(nanl)) ? 1 : 0);
    std::printf("trnanl=%d\n", std::isnan(std::trunc(nanl)) ? 1 : 0);
    volatile long double pinfl = __builtin_huge_vall();
    volatile long double ninfl = -__builtin_huge_vall();
    std::printf("flpinfl=%d\n", std::floor(pinfl) == pinfl ? 1 : 0);
    std::printf("flninfl=%d\n", std::floor(ninfl) == ninfl ? 1 : 0);
    std::printf("cepinfl=%d\n", std::ceil(pinfl) == pinfl ? 1 : 0);
    std::printf("ceninfl=%d\n", std::ceil(ninfl) == ninfl ? 1 : 0);
    std::printf("trpinfl=%d\n", std::trunc(pinfl) == pinfl ? 1 : 0);
    std::printf("trninfl=%d\n", std::trunc(ninfl) == ninfl ? 1 : 0);
    volatile long double lmzero = -0.0L;
    volatile long double flmzl = std::floor(lmzero);
    volatile long double cemzl = std::ceil(lmzero);
    volatile long double trmzl = std::trunc(lmzero);
    std::printf("flmzl=%d\n", (flmzl == 0.0L && std::signbit(flmzl)) ? 1 : 0);
    std::printf("cemzl=%d\n", (cemzl == 0.0L && std::signbit(cemzl)) ? 1 : 0);
    std::printf("trmzl=%d\n", (trmzl == 0.0L && std::signbit(trmzl)) ? 1 : 0);
    return 0;
}
