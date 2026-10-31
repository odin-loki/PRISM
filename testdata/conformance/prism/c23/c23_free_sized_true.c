// PRISM conformance task c23/c23_free_sized_true.c: expected true (memsafety)
#include <stdlib.h>
int c23_free_sized_true(int x) {
    int *p = malloc(4 * sizeof(int));
    if (!p) return 0;
    p[0] = x;
    int r = p[0];
    free_sized(p, 4 * sizeof(int));
    return r;
}
