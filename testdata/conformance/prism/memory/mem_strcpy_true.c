// PRISM conformance task memory/mem_strcpy_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_strcpy_true(void) {
    char d[8];
    strcpy(d, "prism");
    return d[0];
}
