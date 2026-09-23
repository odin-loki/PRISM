// PRISM conformance task memory/mem_ptr_arith_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_ptr_arith_false(int k) {
    int a[4] = {0};
    int *p = a + k;
    return p == a;
}
