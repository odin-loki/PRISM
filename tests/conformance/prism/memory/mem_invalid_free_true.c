// PRISM conformance task memory/mem_invalid_free_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_invalid_free_true(void) {
    int *p = malloc(sizeof(int));
    if (!p) return 0;
    *p = 0;
    int r = *p;
    free(p);
    return r;
}
