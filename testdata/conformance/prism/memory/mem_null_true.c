// PRISM conformance task memory/mem_null_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_null_true(int c) {
    int x = 1;
    int *p = c ? &x : 0;
    return p ? *p : 0;
}
