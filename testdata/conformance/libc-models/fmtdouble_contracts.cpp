/* Exact printf model (src/prism/pir/models/libc/fmtdouble.cpp): setlocale
 * and __prism_fmtv, the renderer the translator calls for sprintf/snprintf
 * with a symbolic double. See harness.h. `_false` ones must fail.
 * Argument windows: ab = 0, ae = 1, pos0 = 0, clsw = class of argument 0
 * (0 i32, 2 double); unused slots are 0. */
#include "harness.h"
#undef N /* a template parameter name in prism_fpfmt.h; not used here */
#include "../../../src/prism/pir/models/libc/fmtdouble.cpp"

#define I8(a) a, 0, 0, 0, 0, 0, 0, 0
#define Z8 0, 0, 0, 0, 0, 0, 0, 0

/* C17 7.11.1.1: "C" is always available; a NULL name queries the category. */
int setlocale_c_true(void) {
    char c[2] = {'C', 0};
    char *r = setlocale(6, c); /* LC_ALL */
    assert(r != 0);
    char *q = setlocale(1, 0); /* LC_NUMERIC */
    assert(q != 0 && q[0] == 'C' && q[1] == 0);
    return 0;
}

/* Wrong contract: a locale that is not installed returns NULL, not a name. */
int setlocale_unknown_false(void) {
    char n[3] = {'x', 'x', 0};
    assert(setlocale(6, n) != 0);
    return 0;
}

/* "%d" of one digit: length 1, the digit and a NUL. */
int fmtv_digit_true(int v) {
    if (v < 0 || v > 9) return 0;
    char buf[4];
    char fmt[3] = {'%', 'd', 0};
    int n = __prism_fmtv(buf, sizeof buf, fmt, 0, 1, 0, 0, I8(v), Z8, Z8, Z8, Z8, Z8);
    assert(n == 1);
    assert(buf[0] == (char)('0' + v) && buf[1] == 0);
    return 0;
}

/* cap 2 holds one character and the NUL: "42" is cut to "4" (length still 2). */
int fmtv_truncated_false(void) {
    char buf[2];
    char fmt[3] = {'%', 'd', 0};
    int n = __prism_fmtv(buf, sizeof buf, fmt, 0, 1, 0, 0, I8(42), Z8, Z8, Z8, Z8, Z8);
    assert(n == 2);
    assert(buf[1] == '2');
    return 0;
}
