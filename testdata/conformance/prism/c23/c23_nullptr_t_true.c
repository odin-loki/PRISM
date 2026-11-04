// PRISM conformance task c23/c23_nullptr_t_true.c: expected true (memsafety)
#include <stddef.h>
int c23_nullptr_t_true(int x) {
    int v = x;
    int *p = (x & 1) ? &v : nullptr;
    nullptr_t n = nullptr;
    return p != n ? *p : 0;
}
