/* <strings.h> ffs and ffsl (src/prism/pir/models/libc/strings_compat.cpp).
 * ffs(0) is 0. The result is one plus the index of the least set bit.
 * A failed assert calls __assert_fail and is FUNC-CONTRACT.
 * This is not PROVED and not CLEAN. */
#include "harness.h"
#include "../../../src/prism/pir/models/libc/strings_compat.cpp"

/* Zero is 0. One, two, and eight name the least set bit: one plus its index.
 * ffsl follows the same rule on long. */
int ffs_least_true(void) {
    assert(ffs(0) == 0);
    assert(ffs(1) == 1);
    assert(ffs(2) == 2);
    assert(ffs(8) == 4);
    assert(ffsl(0L) == 0);
    assert(ffsl(1L) == 1);
    assert(ffsl(2L) == 2);
    assert(ffsl(8L) == 4);
    return 0;
}

/* Wrong contract: ffs(0) is not 1. The failed assert calls __assert_fail. */
int ffs_zero_false(void) {
    assert(ffs(0) == 1);
    return 0;
}
