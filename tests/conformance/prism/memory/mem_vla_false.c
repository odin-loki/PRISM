// PRISM conformance task memory/mem_vla_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_vla_false(int n) {
    if (n < 1 || n > 6) return 0;
    int a[n];
    for (int i = 0; i <= n; i++) a[i] = i;
    return a[0];
}
