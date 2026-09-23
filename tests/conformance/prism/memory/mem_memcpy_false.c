// PRISM conformance task memory/mem_memcpy_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_memcpy_false(int n) {
    char a[16] = {0}, b[8];
    if (n < 0 || n > 16) return 0;
    memcpy(b, a, (size_t)n);
    return 0;
}
