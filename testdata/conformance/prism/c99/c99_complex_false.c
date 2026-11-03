// PRISM conformance task c99/c99_complex_false.c: expected false (no-overflow)
#include <complex.h>
int c99_complex_false(int x) {
    double _Complex z = 1.0 + 2.0 * I;
    return x + (int)cimag(z);
}
