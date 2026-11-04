// PRISM conformance task c11/c11_alignas_true.c: expected true (no-oob)
#include <stdalign.h>
int c11_alignas_true(int x) {
    alignas(16) int a[4] = {0};
    if (x < 0 || x >= (int)(alignof(int))) return 0;
    return a[x];
}
