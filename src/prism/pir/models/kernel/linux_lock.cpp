/* Linux locking, RCU and might_sleep specs (docs/KERNEL_MODELS.md).
 *
 * Reference semantics (include/linux/mutex.h, include/linux/spinlock_api_smp.h,
 * kernel/locking/mutex.c, kernel/locking/spinlock.c, include/linux/rcupdate.h; Linux 6.18):
 *   mutex_lock_nested / mutex_unlock / spin_lock family / rcu_read_lock: take or
 *   release a lock; the calls change no data memory the analysed function can see.
 *   The lock word is not tracked: the model does not check lock balance, double
 *   lock (self-deadlock) or lock ordering, and does not assume a lock is held or
 *   free on entry.
 *   mutex_trylock / spin_trylock: 1 (taken) or 0 (not taken), arbitrary.
 *   mutex_lock_interruptible / killable: 0 (taken) or -EINTR (-4, not taken).
 *   _raw_spin_lock_irqsave: the saved interrupt flags are an arbitrary word.
 * Preconditions checked: the lock pointer is a valid object (a one-byte read of
 * it must be in bounds: NULL, freed and wild lock pointers are reported).
 * Assumptions introduced (named in the verdict):
 *   - lock state is not tracked: no deadlock, lock-balance or lock-order check
 *     (a result with this assumption says nothing about locking discipline)
 *   - lock calls have no effect on data memory (other threads and interrupts
 *     run only while the lock is not held is NOT modelled either: the function
 *     is analysed sequentially, as in all of PRISM's per-function checks)
 *   - might_sleep / lockdep reports have no effect (context checks not modelled)
 */
#include "kernel_model.h"

extern "C" {

static void lock_ptr(const void *lock) {
    ASSUME("linux-lock: lock state is not tracked (no deadlock, lock-balance or lock-order check); lock and unlock calls have no effect on data memory");
    __prism_read_range(lock, 1);
}

/* mutex.h: void mutex_lock_nested(struct mutex *lock, unsigned int subclass) */
void mutex_lock_nested(void *lock, unsigned int subclass) { (void)subclass; lock_ptr(lock); }
/* mutex.h: void mutex_unlock(struct mutex *lock) */
void mutex_unlock(void *lock) { lock_ptr(lock); }
/* mutex.h: void __mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key): the lock starts free */
void __mutex_init(void *lock, const char *name, void *key) { (void)name; (void)key; lock_ptr(lock); }
/* mutex.h: int mutex_trylock(struct mutex *lock): 1 = acquired, 0 = contended */
int mutex_trylock(void *lock) { lock_ptr(lock); return CHOICE() ? 1 : 0; }
/* mutex.h: int mutex_lock_interruptible_nested(struct mutex *lock, unsigned int subclass): 0 or -EINTR */
int mutex_lock_interruptible_nested(void *lock, unsigned int subclass) {
    (void)subclass;
    lock_ptr(lock);
    return CHOICE() ? 0 : -4;
}
/* mutex.h: int _mutex_lock_killable(struct mutex *lock, unsigned int subclass, struct lockdep_map *nest_lock): 0 or -EINTR */
int _mutex_lock_killable(void *lock, unsigned int subclass, void *nest) {
    (void)subclass; (void)nest;
    lock_ptr(lock);
    return CHOICE() ? 0 : -4;
}

/* spinlock_api_smp.h */
void _raw_spin_lock(void *lock) { lock_ptr(lock); }
void _raw_spin_unlock(void *lock) { lock_ptr(lock); }
void _raw_spin_lock_irq(void *lock) { lock_ptr(lock); }
void _raw_spin_unlock_irq(void *lock) { lock_ptr(lock); }
void _raw_spin_lock_bh(void *lock) { lock_ptr(lock); }
void _raw_spin_unlock_bh(void *lock) { lock_ptr(lock); }
/* unsigned long _raw_spin_lock_irqsave(raw_spinlock_t *lock): the previous interrupt flags */
unsigned long _raw_spin_lock_irqsave(void *lock) { lock_ptr(lock); return __VERIFIER_nondet_ulong(); }
unsigned long _raw_spin_lock_irqsave_nested(void *lock, int subclass) {
    (void)subclass;
    lock_ptr(lock);
    return __VERIFIER_nondet_ulong();
}
void _raw_spin_unlock_irqrestore(void *lock, unsigned long flags) { (void)flags; lock_ptr(lock); }
int _raw_spin_trylock(void *lock) { lock_ptr(lock); return CHOICE() ? 1 : 0; }
/* spinlock.h: void __raw_spin_lock_init(raw_spinlock_t *lock, const char *name, struct lock_class_key *key, short inner) */
void __raw_spin_lock_init(void *lock, const char *name, void *key, short inner) {
    (void)name; (void)key; (void)inner;
    lock_ptr(lock);
}

/* rcupdate.h: no effect on the analysed function's memory (read-side critical sections are not tracked) */
void __rcu_read_lock(void) { ASSUME("linux-rcu: RCU read-side sections and grace periods are not modelled (no effect on memory)"); }
void __rcu_read_unlock(void) { ASSUME("linux-rcu: RCU read-side sections and grace periods are not modelled (no effect on memory)"); }
void synchronize_rcu(void) { ASSUME("linux-rcu: RCU read-side sections and grace periods are not modelled (no effect on memory)"); }
/* rcupdate.h (CONFIG_PROVE_RCU): int debug_lockdep_rcu_enabled(void): lockdep's own state, arbitrary */
int debug_lockdep_rcu_enabled(void) { return CHOICE() ? 1 : 0; }
/* rcupdate.h: void lockdep_rcu_suspicious(const char *file, const int line, const char *s): reports, no effect */
void lockdep_rcu_suspicious(const char *file, const int line, const char *s) {
    (void)file; (void)line; (void)s;
    ASSUME("linux-might-sleep: lockdep and might_sleep reports are splats only: no effect, context rules are not checked");
}
/* kernel.h: void __might_sleep(const char *file, int line) */
void __might_sleep(const char *file, int line) {
    (void)file; (void)line;
    ASSUME("linux-might-sleep: lockdep and might_sleep reports are splats only: no effect, context rules are not checked");
}

}  // extern "C"
