// PRISM conformance task memory/mem_uninit_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_uninit_true(int i) {
    int a[4];
    for (int k = 0; k < 4; k++) a[k] = k;
    return a[i & 3];
}
