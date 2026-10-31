// PRISM conformance task c99/c99_complex_true.c: expected true (no-overflow)
#include <complex.h>
int c99_complex_true(short x) {
    double _Complex z = x + 2.0 * I;
    return (int)creal(z) + 1;
}
