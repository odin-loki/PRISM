// PRISM conformance task regress/malloc_abort_true.c: expected true (no-div0)
// regression: F9: abort() on an execution where malloc failed is the
// program's out-of-memory handling, not a defect
#include <stdlib.h>

int malloc_abort_true(int n) {
    int *p = (int *)malloc(sizeof(int));
    if (!p)
        abort();
    *p = n & 7;
    int r = 100 / (*p + 1);
    free(p);
    return r;
}
