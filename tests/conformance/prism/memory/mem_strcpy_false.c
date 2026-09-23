// PRISM conformance task memory/mem_strcpy_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_strcpy_false(void) {
    char d[5];
    strcpy(d, "prism");
    return d[0];
}
