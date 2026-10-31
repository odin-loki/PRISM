// PRISM conformance task c17/c17_aligned_alloc_badalign_false.c: expected false (no-null-deref)
#include <stdlib.h>
int c17_aligned_alloc_badalign_false(int x) {
    int *p = aligned_alloc(3, 12);
    *p = x;
    return 1;
}
