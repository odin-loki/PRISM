// PRISM conformance task macro/limits_false.c: expected false (no-overflow)
#include <limits.h>
int limits_false(int a) {
    if (a == INT_MIN) return a;
    return a + 1;
}
