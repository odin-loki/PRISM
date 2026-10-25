// PRISM conformance task regress/intmin_const_true.c: expected true (no-overflow)
// regression: S6: INT_MIN is the limits.h constant, not a free value
#include <limits.h>
int intmin_const_true(int a) {
    int v = INT_MIN;
    if (a > 0) return v / 2;
    return 0;
}
