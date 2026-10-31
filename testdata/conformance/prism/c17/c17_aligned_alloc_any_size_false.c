// PRISM conformance task c17/c17_aligned_alloc_any_size_false.c: expected false (memsafety)
#include <stdlib.h>
int c17_aligned_alloc_any_size_false(int x) {
    char *p = aligned_alloc(8, 5);
    if (!p) return 0;
    p[5] = (char)x;
    free(p);
    return 0;
}
