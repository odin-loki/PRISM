// PRISM conformance task c23/c23_ckd_add_false.c: expected false (no-overflow)
#include <stdckdint.h>
int c23_ckd_add_false(int a, int b) {
    int r;
    if (ckd_add(&r, a, b)) return a + b;
    return r;
}
