// PRISM conformance task memory/mem_uaf_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_uaf_false(int c) {
    int *p = malloc(sizeof(int));
    if (!p) return 0;
    *p = 5;
    free(p);
    return c ? *p : 0;
}
