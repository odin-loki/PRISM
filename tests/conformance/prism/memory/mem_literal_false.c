// PRISM conformance task memory/mem_literal_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_literal_false(void) {
    char *s = (char *)"abc";
    s[1] = 'x';
    return s[0];
}
