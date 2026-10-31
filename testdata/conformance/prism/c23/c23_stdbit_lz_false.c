// PRISM conformance task c23/c23_stdbit_lz_false.c: expected false (no-shift-ub)
#include <stdbit.h>
int c23_stdbit_lz_false(int x) {
    unsigned s = stdc_leading_zeros((unsigned)x);
    return (int)(1u << s);
}
