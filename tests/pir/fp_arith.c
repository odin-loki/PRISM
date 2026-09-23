/* PIR tasks: IEEE floating point (docs/PIR.md "Floating point").
 * Z3 floating-point theory, round to nearest even; float -> int conversion
 * out of range is undefined behaviour (C11 6.3.1.4p1): FLOAT-CAST-OVF. */
#include <assert.h>
#include <math.h>

int fp_cast_bad(double d) { return (int)d; }

int fp_cast_ok(double d) {
    if (d > -1000.0 && d < 1000.0) return (int)d;
    return 0;
}

int fp_nan_guard_ok(double d) {
    if (d != d) return 0;                      /* NaN */
    if (d >= 2147483647.0 || d <= -2147483649.0) return 0;
    return (int)d;
}

int fp_exact_ok(int x) {
    double d = x;                              /* every int is a double */
    assert((int)d == x);
    return 0;
}

int fp_float_round_bad(int x) {
    if (x < 0 || x > 100000000) return 0;
    float f = (float)x;                        /* 16777217 is not a float */
    assert((int)f == x);
    return 0;
}

int fp_add_ok(float a) {
    if (!(a >= 0.0f && a <= 1000.0f)) return 0;
    float b = a + 1.0f;
    assert(b > a);                             /* exact enough below 2^24 */
    return 1;
}

int fp_add_bad(float a) {
    if (!(a >= 0.0f)) return 0;
    float b = a + 1.0f;
    assert(b > a);                             /* 2^24 + 1 rounds back to 2^24 */
    return 1;
}

double twice_ok(double x) { return x * 2.0; }

unsigned fp_to_unsigned_bad(float f) { return (unsigned)f; }

int fp_half_way_ok(int x) {
    if (x < -1000 || x > 1000) return 0;
    double h = x / 2.0;
    int r = (int)h;                            /* truncates toward zero */
    assert(r == x / 2);
    return r;
}

int fp_sin_ok(double x) {
    double s = sin(x);                         /* unconstrained libm value */
    if (s != s) return 0;
    return s < 2.0;
}

int fp_sin_bad(double x) { return (int)(sin(x) * 1e10); }

double fp_div(double a, double b) { return a / b; } /* FLOAT-DIV-ZERO only with --fp-checks */

double fp_div_guard(double a, double b) {
    if (b == 0.0 || a != a || b != b) return 0.0;
    if (fabs(a) > 1e100 || fabs(b) < 1e-100) return 0.0;
    return a / b;
}
