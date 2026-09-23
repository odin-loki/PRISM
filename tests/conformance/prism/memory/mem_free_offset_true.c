// PRISM conformance task memory/mem_free_offset_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_free_offset_true(void) {
    char *p = malloc(8);
    if (!p) return 0;
    char *q = p + 2;
    free(q - 2);
    return 0;
}
