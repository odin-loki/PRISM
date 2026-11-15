/* Linux locking, RCU and might_sleep specs (docs/KERNEL_MODELS.md, docs/KERNEL_CONCURRENCY.md).
 *
 * Reference semantics (include/linux/mutex.h, include/linux/spinlock_api_smp.h,
 * kernel/locking/mutex.c, kernel/locking/spinlock.c, include/linux/rcupdate.h; Linux 6.18):
 *   mutex_lock_nested / mutex_unlock / spin_lock family / rcu_read_lock: take or
 *   release a lock; the calls change no data memory the analysed function can see.
 *   mutex_trylock / spin_trylock: 1 (taken) or 0 (not taken), arbitrary.
 *   mutex_lock_interruptible / killable: 0 (taken) or -EINTR (-4, not taken).
 *   _raw_spin_lock_irqsave: the saved interrupt flags are an arbitrary word.
 * Preconditions checked: the lock pointer is a valid object (a one-byte read of
 * it must be in bounds: NULL, freed and wild lock pointers are reported).
 *
 * Ghost lock state (per analysed function; docs/KERNEL_CONCURRENCY.md section 2):
 *   __prism_held[] is the set of locks this function has taken and not released;
 *   __prism_atomic counts the spinlocks held. Checks, each a property of its own class:
 *   - LOCK-DOUBLE: a lock taken while already in the set (self-deadlock; mutexes and
 *     spinlocks are not recursive);
 *   - LOCK-UNHELD-UNLOCK: a release of a lock not in the set. A helper that releases
 *     a lock its caller holds is reported here: a model artefact until the module
 *     contract declares the lock held on entry (triage class "missing contract");
 *   - CTX-SLEEP-ATOMIC: a sleeping primitive while a spinlock is held.
 * Not checked here: lock held or free on return, lock order, interrupt and softirq
 * context (LOCK-CTX-MISMATCH), RCU read sections (phase B or the module pass).
 * The set holds PRISM_HELD_MAX locks; past that the state is not tracked and the
 * assumption linux-lock-overflow is recorded.
 *
 * Assumptions introduced (named in the verdict):
 *   - linux-lock: lock calls have no effect on data memory; the function is analysed
 *     sequentially, so other threads and interrupts are not modelled
 *   - linux-lock-overflow: more than PRISM_HELD_MAX locks held at once are not tracked
 *   - linux-might-sleep: lockdep and might_sleep reports (splats) have no effect
 *   - linux-rcu: RCU read-side sections and grace periods are not modelled
 */
#include "kernel_model.h"

#define PRISM_HELD_MAX 8

extern "C" {

/* ghost state: zero at the start of each analysis (static storage) */
static const void *__prism_held[PRISM_HELD_MAX];
static int __prism_atomic;
static int __prism_overflow;

static void lock_ptr(const void *lock) {
    ASSUME("linux-lock: lock calls have no effect on data memory; the function is analysed sequentially");
    __prism_read_range(lock, 1);
}

static int lock_is_held(const void *lock) {
    for (int i = 0; i < PRISM_HELD_MAX; i++)
        if (__prism_held[i] == lock) return 1;
    return 0;
}

/* spin is 1 for a spinlock (atomic context), 0 for a sleeping lock */
static void lock_take(const void *lock, int spin) {
    lock_ptr(lock);
    __prism_check(!lock_is_held(lock), "LOCK-DOUBLE", "lock taken while already held by this function (self-deadlock)");
    int placed = 0;
    for (int i = 0; i < PRISM_HELD_MAX && !placed; i++)
        if (__prism_held[i] == 0) { __prism_held[i] = lock; placed = 1; }
    if (!placed && !__prism_overflow) {
        __prism_overflow = 1;
        ASSUME("linux-lock-overflow: more than PRISM_HELD_MAX locks held at once are not tracked");
    }
    if (spin) __prism_atomic++;
}

static void lock_drop(const void *lock, int spin) {
    lock_ptr(lock);
    int found = 0;
    for (int i = 0; i < PRISM_HELD_MAX && !found; i++)
        if (__prism_held[i] == lock) { __prism_held[i] = 0; found = 1; }
    __prism_check(found || __prism_overflow, "LOCK-UNHELD-UNLOCK", "lock released that this function does not hold");
    if (spin) __prism_atomic--;
}

/* a sleeping primitive: legal only with no spinlock held */
static void sleep_point(const char *what) {
    __prism_check(__prism_atomic <= 0, "CTX-SLEEP-ATOMIC", what);
}

/* mutex.h: void mutex_lock_nested(struct mutex *lock, unsigned int subclass) */
void mutex_lock_nested(void *lock, unsigned int subclass) {
    (void)subclass;
    sleep_point("mutex_lock: may sleep while a spinlock is held");
    lock_take(lock, 0);
}
/* mutex.h: void mutex_unlock(struct mutex *lock) */
void mutex_unlock(void *lock) { lock_drop(lock, 0); }
/* mutex.h: void __mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key): the lock starts free */
void __mutex_init(void *lock, const char *name, void *key) { (void)name; (void)key; lock_ptr(lock); }
/* mutex.h: int mutex_trylock(struct mutex *lock): 1 = acquired, 0 = contended (never sleeps) */
int mutex_trylock(void *lock) {
    lock_ptr(lock);
    if (CHOICE()) { lock_take(lock, 0); return 1; }
    return 0;
}
/* mutex.h: int mutex_lock_interruptible_nested(struct mutex *lock, unsigned int subclass): 0 or -EINTR */
int mutex_lock_interruptible_nested(void *lock, unsigned int subclass) {
    (void)subclass;
    sleep_point("mutex_lock_interruptible: may sleep while a spinlock is held");
    if (CHOICE()) { lock_take(lock, 0); return 0; }
    lock_ptr(lock);
    return -4;
}
/* mutex.h: int _mutex_lock_killable(struct mutex *lock, unsigned int subclass, struct lockdep_map *nest_lock): 0 or -EINTR */
int _mutex_lock_killable(void *lock, unsigned int subclass, void *nest) {
    (void)subclass; (void)nest;
    sleep_point("mutex_lock_killable: may sleep while a spinlock is held");
    if (CHOICE()) { lock_take(lock, 0); return 0; }
    lock_ptr(lock);
    return -4;
}

/* spinlock_api_smp.h */
void _raw_spin_lock(void *lock) { lock_take(lock, 1); }
void _raw_spin_unlock(void *lock) { lock_drop(lock, 1); }
void _raw_spin_lock_irq(void *lock) { lock_take(lock, 1); }
void _raw_spin_unlock_irq(void *lock) { lock_drop(lock, 1); }
void _raw_spin_lock_bh(void *lock) { lock_take(lock, 1); }
void _raw_spin_unlock_bh(void *lock) { lock_drop(lock, 1); }
/* unsigned long _raw_spin_lock_irqsave(raw_spinlock_t *lock): the previous interrupt flags */
unsigned long _raw_spin_lock_irqsave(void *lock) { lock_take(lock, 1); return __VERIFIER_nondet_ulong(); }
unsigned long _raw_spin_lock_irqsave_nested(void *lock, int subclass) {
    (void)subclass;
    lock_take(lock, 1);
    return __VERIFIER_nondet_ulong();
}
void _raw_spin_unlock_irqrestore(void *lock, unsigned long flags) { (void)flags; lock_drop(lock, 1); }
int _raw_spin_trylock(void *lock) {
    lock_ptr(lock);
    if (CHOICE()) { lock_take(lock, 1); return 1; }
    return 0;
}
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
    ASSUME("linux-might-sleep: lockdep and might_sleep reports are splats only: no effect");
}
/* kernel.h: void __might_sleep(const char *file, int line) */
void __might_sleep(const char *file, int line) {
    (void)file; (void)line;
    ASSUME("linux-might-sleep: lockdep and might_sleep reports are splats only: no effect");
    sleep_point("might_sleep: sleeping while a spinlock is held");
}

}  // extern "C"
