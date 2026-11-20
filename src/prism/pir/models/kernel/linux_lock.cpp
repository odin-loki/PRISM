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
 * Ghost lock state (translate_lock.inc, docs/KERNEL_CONCURRENCY.md section 2.1): the
 * intrinsics below keep the set of locks the analysed function holds and the
 * atomic depth. They are translator intrinsics, not C code: no body is linked.
 *   __prism_lock_acquire(p, spin)  LOCK-DOUBLE if p is already held;
 *   __prism_lock_release(p, spin)  LOCK-UNHELD-UNLOCK if p is not held;
 *   __prism_sleep_point()          SLEEP-IN-ATOMIC if a spinlock or preempt count is held.
 *   __prism_atomic_add(delta)      preempt / bh count += delta (PREEMPT-UNBALANCED below 0)
 *   __prism_rcu_add(delta)         RCU nesting += delta (RCU-UNBALANCED below 0)
 *   __prism_sleep_if(may)          SLEEP-IN-ATOMIC / RCU-SLEEP when may is set
 * A helper that releases a lock its caller holds is reported as LOCK-UNHELD-UNLOCK
 * unless its module contract says so (__prism_lock_entry, translate_lock.inc). Not
 * checked here: lock order, interrupt and softirq context, RCU grace periods.
 *
 * Assumptions introduced (named in the verdict):
 *   - linux-lock: lock calls have no effect on data memory; the function is analysed
 *     sequentially, so other threads and interrupts are not modelled
 *   - linux-lock-overflow: more than 8 locks held at once: the set overflows and the
 *     release check is then not reported (recorded by translate_lock.inc)
 *   - linux-lock-contract: the declared entry and exit locks are the caller's contract
 *   - linux-preempt: preempt and bh counts are modelled; local_irq_save is not (inline asm)
 *   - linux-might-sleep: lockdep and might_sleep reports (splats) have no effect
 *   - linux-rcu: RCU grace periods are not modelled; read-side nesting is counted
 */
#include "kernel_model.h"

extern "C" {

/* translator intrinsics (translate.cpp, translate_lock.inc): not linked */
void __prism_lock_acquire(const void *lock, int spin);
void __prism_lock_release(const void *lock, int spin);
void __prism_sleep_point(void);
void __prism_sleep_if(int may);
void __prism_atomic_add(int delta);
void __prism_rcu_add(int delta);

static void lock_ptr(const void *lock) {
    ASSUME("linux-lock: lock calls have no effect on data memory; the function is analysed sequentially");
    __prism_read_range(lock, 1);
}

static void lock_take(const void *lock, int spin) {
    lock_ptr(lock);
    __prism_lock_acquire(lock, spin);
}

static void lock_drop(const void *lock, int spin) {
    lock_ptr(lock);
    __prism_lock_release(lock, spin);
}

/* mutex.h: void mutex_lock_nested(struct mutex *lock, unsigned int subclass) */
void mutex_lock_nested(void *lock, unsigned int subclass) {
    (void)subclass;
    __prism_sleep_point();
    lock_take(lock, 0);
}
/* mutex.h: void mutex_unlock(struct mutex *lock) */
void mutex_unlock(void *lock) { lock_drop(lock, 0); }
/* mutex.h: void __mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key): the lock starts free */
void __mutex_init(void *lock, const char *name, void *key) { (void)name; (void)key; lock_ptr(lock); }
/* mutex.h: int mutex_trylock(struct mutex *lock): 1 = acquired, 0 = contended (never sleeps) */
int mutex_trylock(void *lock) {
    lock_ptr(lock);
    if (CHOICE()) { __prism_lock_acquire(lock, 0); return 1; }
    return 0;
}
/* mutex.h: int mutex_lock_interruptible_nested(struct mutex *lock, unsigned int subclass): 0 or -EINTR */
int mutex_lock_interruptible_nested(void *lock, unsigned int subclass) {
    (void)subclass;
    __prism_sleep_point();
    lock_ptr(lock);
    if (CHOICE()) { __prism_lock_acquire(lock, 0); return 0; }
    return -4;
}
/* mutex.h: int _mutex_lock_killable(struct mutex *lock, unsigned int subclass, struct lockdep_map *nest_lock): 0 or -EINTR */
int _mutex_lock_killable(void *lock, unsigned int subclass, void *nest) {
    (void)subclass; (void)nest;
    __prism_sleep_point();
    lock_ptr(lock);
    if (CHOICE()) { __prism_lock_acquire(lock, 0); return 0; }
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
    if (CHOICE()) { __prism_lock_acquire(lock, 1); return 1; }
    return 0;
}
/* spinlock.h: void __raw_spin_lock_init(raw_spinlock_t *lock, const char *name, struct lock_class_key *key, short inner) */
void __raw_spin_lock_init(void *lock, const char *name, void *key, short inner) {
    (void)name; (void)key; (void)inner;
    lock_ptr(lock);
}

/* rcupdate.h: no effect on the analysed function's memory; the read-side nesting is counted (RCU-UNBALANCED, RCU-HELD-ON-RETURN) */
void __rcu_read_lock(void) { ASSUME("linux-rcu: RCU grace periods are not modelled; read-side nesting is counted (no effect on memory)"); __prism_rcu_add(1); }
void __rcu_read_unlock(void) { ASSUME("linux-rcu: RCU grace periods are not modelled; read-side nesting is counted (no effect on memory)"); __prism_rcu_add(-1); }
void synchronize_rcu(void) { ASSUME("linux-rcu: RCU read-side sections and grace periods are not modelled (no effect on memory)"); __prism_sleep_point(); }

/* preempt.h: the preempt count (preempt_disable is preempt_count_add(1) through the header macros) */
void preempt_count_add(int val) { ASSUME("linux-preempt: preempt count is counted, not its per-CPU storage (no effect on memory)"); __prism_atomic_add(val); }
void preempt_count_sub(int val) { ASSUME("linux-preempt: preempt count is counted, not its per-CPU storage (no effect on memory)"); __prism_atomic_add(-val); }
/* bottom halves (softirq.c): local_bh_disable adds cnt to the preempt count; local_bh_enable removes it */
void __local_bh_disable_ip(unsigned long ip, unsigned int cnt) { (void)ip; ASSUME("linux-preempt: bh disable is counted in the preempt count (no effect on memory)"); __prism_atomic_add((int)cnt); }
void __local_bh_enable_ip(unsigned long ip, unsigned int cnt) { (void)ip; ASSUME("linux-preempt: bh disable is counted in the preempt count (no effect on memory)"); __prism_atomic_add(-(int)cnt); }
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
    __prism_sleep_point();
}

}  // extern "C"
