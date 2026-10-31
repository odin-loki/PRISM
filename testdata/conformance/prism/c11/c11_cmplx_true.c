// PRISM conformance task c11/c11_cmplx_true.c: expected true (no-overflow)
#include <complex.h>
#ifndef CMPLX /* glibc defines it only with __builtin_complex (GCC) */
#define CMPLX(x, y) ((double _Complex){(x), (y)})
#endif
int c11_cmplx_true(short x) {
    double _Complex z = CMPLX((double)x, 1.0);
    return (int)creal(z) + (int)cimag(z);
}
