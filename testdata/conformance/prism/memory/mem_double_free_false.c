// PRISM conformance task memory/mem_double_free_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_double_free_false(int c) {
    char *p = malloc(8);
    if (!p) return 0;
    free(p);
    if (c) free(p);
    return 0;
}
