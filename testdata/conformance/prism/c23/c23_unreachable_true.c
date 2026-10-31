// PRISM conformance task c23/c23_unreachable_true.c: expected true (noassert)
#include <stddef.h>
int c23_unreachable_true(int x) {
    if (x == 0) return 0;
    if (x < -1000000 || x > 1000000) return 1;
    return 100 / x;
}
