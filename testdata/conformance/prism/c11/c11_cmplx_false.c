// PRISM conformance task c11/c11_cmplx_false.c: expected false (no-overflow)
#include <complex.h>
#ifndef CMPLX /* glibc defines it only with __builtin_complex (GCC) */
#define CMPLX(x, y) ((double _Complex){(x), (y)})
#endif
int c11_cmplx_false(int x) {
    double _Complex z = CMPLX(1.0, 1.0);
    return x + (int)cimag(z);
}
