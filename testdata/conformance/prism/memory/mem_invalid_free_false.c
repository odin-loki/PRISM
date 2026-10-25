// PRISM conformance task memory/mem_invalid_free_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_invalid_free_false(void) {
    int x = 0;
    free(&x);
    return x;
}
