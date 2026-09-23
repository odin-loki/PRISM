// PRISM conformance task memory/mem_memcpy_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_memcpy_true(int n) {
    char a[8] = {0}, b[8];
    if (n < 0 || n > 8) return 0;
    memcpy(b, a, (size_t)n);
    return 0;
}
