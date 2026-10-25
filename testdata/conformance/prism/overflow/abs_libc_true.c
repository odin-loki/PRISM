// PRISM conformance task overflow/abs_libc_true.c: expected true (no-overflow)
#include <stdlib.h>
int abs_libc_true(int a) {
    if (a < -1000 || a > 1000) return 0;
    return abs(a);
}
