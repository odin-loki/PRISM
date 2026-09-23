// PRISM conformance task memory/mem_literal_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_literal_true(void) {
    char s[] = "abc";
    s[1] = 'x';
    return s[0];
}
