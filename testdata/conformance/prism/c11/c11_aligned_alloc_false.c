// PRISM conformance task c11/c11_aligned_alloc_false.c: expected false (memsafety)
#include <stdlib.h>
int c11_aligned_alloc_false(int x) {
    int *p = aligned_alloc(16, 16);
    if (!p) return 0;
    p[x & 7] = 1;
    free(p);
    return 0;
}
