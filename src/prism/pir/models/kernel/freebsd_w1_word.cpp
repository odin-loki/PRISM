/* FreeBSD 32-bit user word access and critical-section specs (docs/KERNEL_MODELS.md), W1 group f1.
 *
 * Reference semantics (sys/sys/systm.h, suword(9); sys/sys/critical.h, critical(9); FreeBSD 15):
 *   int suword32(volatile void *base, int32_t word): stores the 32-bit word at the user
 *     address base; returns 0 on success, -1 on a fault. Nothing is stored on a fault.
 *   int fueword32(volatile const void *base, int32_t *val): loads the 32-bit word at the
 *     user address base into *val; returns 0 on success, -1 on a fault. *val is written
 *     only on success.
 *   critical_enter / critical_exit: enter / leave a critical section (preemption disabled).
 * Failure paths: a user access may fault; the model chooses success or failure arbitrarily
 *   because user mapping state is not tracked. A fault never crashes the kernel.
 * Assumptions introduced (named in the verdict):
 *   - user mapping state is not tracked: suword32 / fueword32 succeed or fail (-1) arbitrarily
 *   - the value fueword32 loads is arbitrary
 *   - critical-section nesting is not tracked (no balance check)
 * Not specified (see NEEDS REVIEW in docs/KERNEL_MODELS.md): __sx_xlock, __sx_xunlock,
 *   sched_prio, sched_relinquish, calculate_crc32c (truncated prototype), vm_map_max.
 */
#include "kernel_model.h"

extern "C" {

/* int suword32(volatile void *base, int32_t word) (sys/sys/systm.h, suword(9)) */
int suword32(volatile void *base, int word) {
    int r = CHOICE() ? 0 : -1;
    if (r == 0) {
        ASSUME("freebsd-f1-suword32: stores 4 bytes at the user address on success (0); a fault (-1) stores nothing; the user mapping is not tracked, so the outcome is arbitrary");
        __prism_memcpy((void *)base, &word, 4, 0);
    } else {
        ASSUME("freebsd-f1-suword32: stores 4 bytes at the user address on success (0); a fault (-1) stores nothing; the user mapping is not tracked, so the outcome is arbitrary");
    }
    return r;
}

/* int fueword32(volatile const void *base, int32_t *val) (sys/sys/systm.h, suword(9)) */
int fueword32(volatile const void *base, int *val) {
    (void)base;
    int r = CHOICE() ? 0 : -1;
    if (r == 0) {
        ASSUME("freebsd-f1-fueword32: loads an arbitrary 4-byte value into *val on success (0); a fault (-1) leaves *val untouched; the user mapping is not tracked");
        *val = __VERIFIER_nondet_int();
    } else {
        ASSUME("freebsd-f1-fueword32: loads an arbitrary 4-byte value into *val on success (0); a fault (-1) leaves *val untouched; the user mapping is not tracked");
    }
    return r;
}

/* void critical_enter(void) (sys/sys/critical.h, critical(9)) */
void critical_enter(void) {
    ASSUME("freebsd-f1-critical_enter: enters a critical section (preemption disabled); no effect on data memory, nesting not tracked");
}

/* void critical_exit(void) (sys/sys/critical.h, critical(9)) */
void critical_exit(void) {
    ASSUME("freebsd-f1-critical_exit: leaves the critical section; no effect on data memory, nesting not tracked");
}

}  // extern "C"
