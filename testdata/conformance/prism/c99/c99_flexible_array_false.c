// PRISM conformance task c99/c99_flexible_array_false.c: expected false (memsafety)
#include <stdlib.h>
struct buf { int n; int d[]; };
int c99_flexible_array_false(int x) {
    if (x < 1 || x > 8) return 0;
    struct buf *b = malloc(sizeof *b + (size_t)x * sizeof(int));
    if (!b) return 0;
    b->d[x] = 7;
    free(b);
    return 0;
}
