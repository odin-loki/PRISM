// PRISM conformance task c23/c23_nullptr_t_false.c: expected false (memsafety)
#include <stddef.h>
int c23_nullptr_t_false(int x) {
    int v = x;
    int *p = (x & 1) ? &v : nullptr;
    return *p;
}
