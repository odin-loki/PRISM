// PRISM conformance task c23/c23_ckd_add_true.c: expected true (no-overflow)
// ckd_add reports overflow instead of performing UB
#include <stdckdint.h>
int c23_ckd_add_true(int a, int b) {
    int r;
    if (ckd_add(&r, a, b)) return 0;
    return r;
}
