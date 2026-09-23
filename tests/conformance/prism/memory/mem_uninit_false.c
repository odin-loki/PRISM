// PRISM conformance task memory/mem_uninit_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_uninit_false(int i) {
    int a[4];
    a[0] = 1;
    return a[i & 3];
}
