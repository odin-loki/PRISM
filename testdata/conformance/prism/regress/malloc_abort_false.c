// PRISM conformance task regress/malloc_abort_false.c: expected false (no-div0)
// regression: F9: after the out-of-memory abort() the rest is still checked
#include <stdlib.h>

int malloc_abort_false(int n) {
    int *p = (int *)malloc(sizeof(int));
    if (!p)
        abort();
    *p = n & 7;
    int r = 100 / (*p - 3);
    free(p);
    return r;
}
