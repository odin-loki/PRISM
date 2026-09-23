// PRISM conformance task regress/intmin_const_false.c: expected false (no-overflow)
// regression: S6: INT_MIN is the limits.h constant, not a free value
#include <limits.h>
int intmin_const_false(int a) {
    int v = INT_MIN;
    if (a > 0) return v * 2;
    return 0;
}
