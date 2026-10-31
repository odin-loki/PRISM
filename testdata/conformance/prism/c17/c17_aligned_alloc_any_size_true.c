// PRISM conformance task c17/c17_aligned_alloc_any_size_true.c: expected true (memsafety)
#include <stdlib.h>
/* C17 (DR 460): size need not be a multiple of the alignment */
int c17_aligned_alloc_any_size_true(int x) {
    char *p = aligned_alloc(8, 5);
    if (!p) return 0;
    p[4] = (char)x;
    int r = p[4];
    free(p);
    return r;
}
