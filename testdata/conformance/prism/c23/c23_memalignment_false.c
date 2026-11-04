// PRISM conformance task c23/c23_memalignment_false.c: expected false (no-div0)
#include <stdlib.h>
int c23_memalignment_false(int x) {
    return x / (int)memalignment((void *)0);
}
