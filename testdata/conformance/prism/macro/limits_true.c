// PRISM conformance task macro/limits_true.c: expected true (no-overflow)
#include <limits.h>
int limits_true(int a) {
    if (a == INT_MAX) return a;
    return a + 1;
}
