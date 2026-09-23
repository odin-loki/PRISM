// PRISM conformance task memory/mem_realloc_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_realloc_true(void) {
    char *p = malloc(2);
    if (!p) return 0;
    p[0] = 1;
    char *q = realloc(p, 8);
    if (!q) {
        free(p);
        return 0;
    }
    q[7] = 2;
    int r = q[0];
    free(q);
    return r;
}
