// PRISM conformance task memory/mem_realloc_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_realloc_false(void) {
    char *p = malloc(2);
    if (!p) return 0;
    p[0] = 1;
    char *q = realloc(p, 8);
    if (!q) {
        free(p);
        return 0;
    }
    int r = p[0];
    free(q);
    return r;
}
