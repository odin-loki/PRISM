// PRISM conformance task memory/mem_free_offset_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_free_offset_false(void) {
    char *p = malloc(8);
    if (!p) return 0;
    free(p + 2);
    return 0;
}
