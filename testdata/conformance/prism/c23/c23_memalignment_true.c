// PRISM conformance task c23/c23_memalignment_true.c: expected true (no-div0)
#include <stdlib.h>
int c23_memalignment_true(int x) {
    int v = 0;
    size_t a = memalignment(&v);
    return x / (int)(a ? 1 : 1);
}
