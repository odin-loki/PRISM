// PRISM conformance task c23/c23_stdbit_lz_true.c: expected true (no-shift-ub)
#include <stdbit.h>
int c23_stdbit_lz_true(int x) {
    unsigned s = stdc_leading_zeros((unsigned)x);
    return (int)(1u << (s & 31u));
}
