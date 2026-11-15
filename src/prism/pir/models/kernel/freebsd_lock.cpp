/* FreeBSD mutex and sx lock specs (docs/KERNEL_MODELS.md).
 *
 * Reference semantics (sys/sys/mutex.h, sys/sys/sx.h, sys/kern/kern_mutex.c,
 * sys/kern/kern_sx.c, mutex(9), sx(9); FreeBSD 15): acquire or release a lock
 * (the mtx_lock/mtx_unlock/sx_xlock macros expand to these calls); the calls
 * change no data memory the analysed function can see. The lock word is not
 * tracked: no check of lock balance, recursion, lock order or sleep-with-mutex
 * (WITNESS). mtx_trylock: nonzero (acquired) or 0, arbitrary. _sx_xlock/_sx_slock:
 * 0, or an error when the lock is interruptible (SX_INTERRUPTIBLE): arbitrary int.
 * Preconditions checked: the lock word is valid memory (8 bytes in bounds).
 * Assumptions introduced (named in the verdict):
 *   - lock state is not tracked (no deadlock, balance or order check)
 *   - lock calls have no effect on data memory
 * Not specified here (needs review): rw locks (__rw_wlock_hard, _rw_wunlock_cookie),
 * the argument layout of the lock-debug variants (LOCK_DEBUG > 0) of
 * __mtx_lock_sleep / __mtx_unlock_sleep (an arity mismatch stops at UNENCODED),
 * cv_wait, msleep.
 */
#include "kernel_model.h"

extern "C" {

static void fb_lock(const void *c) {
    ASSUME("freebsd-lock: lock state is not tracked (no deadlock, balance or order check); lock and unlock calls have no effect on data memory");
    __prism_read_range(c, 8);
}

/* void __mtx_lock_flags(volatile uintptr_t *c, int opts, const char *file, int line) */
void __mtx_lock_flags(void *c, int opts, const char *file, int line) { (void)opts; (void)file; (void)line; fb_lock(c); }
void __mtx_unlock_flags(void *c, int opts, const char *file, int line) { (void)opts; (void)file; (void)line; fb_lock(c); }
/* void __mtx_lock_sleep(volatile uintptr_t *c, uintptr_t v) (LOCK_DEBUG == 0 form) */
void __mtx_lock_sleep(void *c, unsigned long v) { (void)v; fb_lock(c); }
void __mtx_unlock_sleep(void *c, unsigned long v) { (void)v; fb_lock(c); }
/* void _mtx_init(volatile uintptr_t *c, const char *name, const char *type, int opts) */
void _mtx_init(void *c, const char *name, const char *type, int opts) { (void)name; (void)type; (void)opts; fb_lock(c); }
void _mtx_destroy(void *c) { fb_lock(c); }
/* int _mtx_trylock_flags_(volatile uintptr_t *c, int opts, const char *file, int line) */
int _mtx_trylock_flags_(void *c, int opts, const char *file, int line) {
    (void)opts; (void)file; (void)line;
    fb_lock(c);
    return CHOICE() ? 1 : 0;
}
/* sx.h */
void sx_init_flags(void *sx, const char *description, int opts) { (void)description; (void)opts; fb_lock(sx); }
void sx_destroy(void *sx) { fb_lock(sx); }
int _sx_xlock(void *sx, int opts, const char *file, int line) { (void)opts; (void)file; (void)line; fb_lock(sx); return __VERIFIER_nondet_int(); }
int _sx_slock(void *sx, int opts, const char *file, int line) { (void)opts; (void)file; (void)line; fb_lock(sx); return __VERIFIER_nondet_int(); }
void _sx_xunlock(void *sx, const char *file, int line) { (void)file; (void)line; fb_lock(sx); }
void _sx_sunlock(void *sx, const char *file, int line) { (void)file; (void)line; fb_lock(sx); }

}  // extern "C"
