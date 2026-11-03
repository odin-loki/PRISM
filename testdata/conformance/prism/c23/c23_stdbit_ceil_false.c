// PRISM conformance task c23/c23_stdbit_ceil_false.c: expected false (no-oob)
#include <stdbit.h>
int c23_stdbit_ceil_false(int x) {
    unsigned a[16] = {0};
    unsigned v = (unsigned)x & 15u;
    return (int)a[stdc_bit_ceil(v)];
}
