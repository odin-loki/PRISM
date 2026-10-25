// PRISM conformance task memory/mem_null_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_null_false(int c) {
    int x = 1;
    int *p = c ? &x : 0;
    return *p;
}
