// PRISM conformance task overflow/abs_libc_false.c: expected false (no-overflow)
// abs(INT_MIN) is undefined (7.22.6.1p2)
#include <stdlib.h>
int abs_libc_false(int a) {
    return abs(a);
}
