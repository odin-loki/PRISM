// PRISM conformance task memory/mem_uaf_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_uaf_true(int c) {
    int *p = malloc(sizeof(int));
    if (!p) return 0;
    *p = c & 7;
    int r = c ? *p : 0;
    free(p);
    return r;
}
