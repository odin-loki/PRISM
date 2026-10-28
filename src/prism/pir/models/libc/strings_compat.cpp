/* <strings.h> ffs and ffsl (docs/PIR.md "Library models").
 * ffs(0) is 0. ffs(1) is 1. ffs(2) is 2. ffs(8) is 4.
 * The result is one plus the index of the least set bit, and it is never a wrapped negative.
 * ffsl follows the same rule on long.
 * This is not PROVED and not CLEAN. */
#include "prism_model.h"

extern "C" {

/* Zero returns 0. A nonzero value returns one plus the index of its least set bit.
 * The argument is converted to unsigned before the scan, so the scan does not
 * negate it. bit stays below 64, so bit + 1 is never a wrapped negative. */
int ffs(int i) {
    /* Convert to unsigned. Do not negate. bit + 1 is never a wrapped negative. */
    unsigned int x = (unsigned int)i;
    if (x == 0u) return 0;
    int bit = 0;
    while (bit < 63 && (x & 1u) == 0u) {
        x >>= 1;
        bit += 1;
    }
    return bit + 1;
}

/* Same rule on long. The argument is converted to unsigned long before the scan. */
int ffsl(long i) {
    /* Convert to unsigned long. Do not negate. bit + 1 is never a wrapped negative. */
    unsigned long x = (unsigned long)i;
    if (x == 0ul) return 0;
    int bit = 0;
    while (bit < 63 && (x & 1ul) == 0ul) {
        x >>= 1;
        bit += 1;
    }
    return bit + 1;
}

}  // extern "C"
