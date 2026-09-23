// PRISM conformance task memory/mem_stack_oob_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_stack_oob_true(int i) {
    int a[8] = {0};
    if (i < 0 || i >= 8) return 0;
    a[i] = 1;
    return a[0];
}
