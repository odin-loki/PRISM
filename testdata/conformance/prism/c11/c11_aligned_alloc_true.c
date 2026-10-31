// PRISM conformance task c11/c11_aligned_alloc_true.c: expected true (memsafety)
#include <stdlib.h>
int c11_aligned_alloc_true(int x) {
    int *p = aligned_alloc(16, 16);
    if (!p) return 0;
    p[3] = x;
    int r = p[3];
    free(p);
    return r;
}
