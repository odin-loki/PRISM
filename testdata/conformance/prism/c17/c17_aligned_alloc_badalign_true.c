// PRISM conformance task c17/c17_aligned_alloc_badalign_true.c: expected true (no-null-deref)
#include <stdlib.h>
/* C17: an unsupported alignment returns NULL; the check handles it */
int c17_aligned_alloc_badalign_true(int x) {
    int *p = aligned_alloc(3, 12);
    if (!p) return 0;
    *p = x;
    free(p);
    return 1;
}
