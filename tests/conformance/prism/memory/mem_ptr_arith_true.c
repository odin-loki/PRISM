// PRISM conformance task memory/mem_ptr_arith_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_ptr_arith_true(int k) {
    int a[4] = {0};
    if (k < 0 || k > 4) return 0;
    int *p = a + k;
    return p == a;
}
