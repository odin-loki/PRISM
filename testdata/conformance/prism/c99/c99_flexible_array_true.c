// PRISM conformance task c99/c99_flexible_array_true.c: expected true (memsafety)
#include <stdlib.h>
struct buf { int n; int d[]; };
int c99_flexible_array_true(int x) {
    if (x < 1 || x > 8) return 0;
    struct buf *b = malloc(sizeof *b + (size_t)x * sizeof(int));
    if (!b) return 0;
    b->n = x;
    b->d[x - 1] = 7;
    int r = b->d[x - 1];
    free(b);
    return r;
}
