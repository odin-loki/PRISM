// PRISM conformance task memory/mem_heap_oob_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_heap_oob_true(int i) {
    int *p = malloc(4 * sizeof(int));
    if (!p) return 0;
    if (i >= 0 && i < 4) p[i] = 1;
    free(p);
    return 0;
}
