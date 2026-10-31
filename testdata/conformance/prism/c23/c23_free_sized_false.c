// PRISM conformance task c23/c23_free_sized_false.c: expected false (memsafety)
#include <stdlib.h>
int c23_free_sized_false(int x) {
    int *p = malloc(16);
    if (!p) return 0;
    free_sized(p, (unsigned)(x & 31));
    return 0;
}
