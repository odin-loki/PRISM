// PRISM conformance task c23/c23_unreachable_false.c: expected false (noassert)
#include <stddef.h>
int c23_unreachable_false(int x) {
    if (x != 0) return 1;
    if (x == 0) unreachable();
    return 0;
}
