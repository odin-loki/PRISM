/* FreeBSD user-space word and byte access specs (docs/KERNEL_MODELS.md), W1 groups f2 and f3.
 *
 * Reference semantics (sys/sys/systm.h, store(9), fueword(9); FreeBSD 15):
 *   int fueword(volatile const void *base, long *val): fetches the 8-byte word at the
 *     user address base into *val; returns 0 on success, -1 when the load faults.
 *     *val is written only on success. The result is __result_use_check.
 *   int suword(volatile void *base, long word), int subyte(volatile void *base, int byte):
 *     store the long / the low byte at the user address base; return 0 on success and -1
 *     when the store faults (store(9)). The result is __nodiscard.
 * Failure paths: any of the three may fault; the model chooses success or failure
 *   arbitrarily because user-address validity is not tracked.
 * Preconditions checked by the memory model: on the success path, *val (fueword) or
 *   base (suword, subyte) is writable for the stored size.
 * Assumptions introduced (named in the verdict):
 *   - user memory is not tracked: the fetched word is arbitrary, and whether a user
 *     access faults is an arbitrary choice
 *   - user addresses are not tracked: a store on the success path is checked only for
 *     writability of the stored size
 * Not specified (see NEEDS REVIEW in docs/KERNEL_MODELS.md): vm_map_min and vm_map_max
 *   (macros in sys/vm/vm_map.h, not functions), and the scheduler, casueword32, fubyte
 *   and fillw entries (no prototype or semantics not certain).
 */
#include "kernel_model.h"

extern "C" {

/* int __result_use_check fueword(volatile const void *base, long *val) (sys/sys/systm.h) */
int fueword(const volatile void *base, long *val) {
    (void)base;
    if (CHOICE()) { /* fault path: *val is left untouched */
        ASSUME("freebsd-f3-fueword: the user-space load may fault; the validity of the user address is not tracked, so the fault (-1) vs success (0) outcome is arbitrary");
        return -1;
    }
    ASSUME("freebsd-f3-fueword: the fetched word is arbitrary (user memory contents are not tracked)");
    unsigned long hi = (unsigned int)__VERIFIER_nondet_int();
    unsigned long lo = (unsigned int)__VERIFIER_nondet_int();
    *val = (long)((hi << 32) | lo);
    return 0;
}

/* __nodiscard int suword(volatile void *base, long word) (sys/sys/systm.h, store(9)) */
int suword(volatile void *base, long word) {
    if (CHOICE()) {
        ASSUME("freebsd-f2-suword: success path - the long is stored at base and 0 is returned; that base is valid writable user memory is chosen arbitrarily (user addresses are not tracked)");
        __prism_memcpy((void *)base, &word, sizeof(word), 0);
        return 0;
    }
    ASSUME("freebsd-f2-suword: failure path - nothing is stored and -1 is returned (store(9)); the fault itself is an arbitrary choice");
    return -1;
}

/* __nodiscard int subyte(volatile void *base, int byte) (sys/sys/systm.h, store(9)) */
int subyte(volatile void *base, int byte) {
    if (CHOICE()) {
        ASSUME("freebsd-f2-subyte: success path - the low byte of byte is stored at base and 0 is returned; that base is valid writable user memory is chosen arbitrarily (user addresses are not tracked)");
        char b = (char)byte;
        __prism_memcpy((void *)base, &b, 1, 0);
        return 0;
    }
    ASSUME("freebsd-f2-subyte: failure path - nothing is stored and -1 is returned (store(9)); the fault itself is an arbitrary choice");
    return -1;
}

}  // extern "C"
