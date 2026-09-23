// PRISM conformance task memory/mem_double_free_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_double_free_true(int c) {
    char *p = malloc(8);
    if (!p) return 0;
    if (c) {
        free(p);
        p = 0;
    }
    free(p);
    return 0;
}
