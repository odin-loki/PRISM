/* Lock contracts (docs/KERNEL_CONCURRENCY.md section 2.2, W7r step 1). Run with `--kernel-api linux`.
 * __prism_lock_entry(p, spin) declares a lock the caller holds on entry; __prism_lock_exit(p)
 * declares a lock the function returns with held. Each *_ok function is PROVED-ASSUMING
 * (linux-lock-contract); each *_bad function is FAILED with the class in its name.
 * The locks are file-scope objects, not parameters: a pointer parameter of an analysed
 * root is NEEDS-HARNESS (its precondition is unknown), as for every kernel_*.c table.
 * The markers are read only in the analysed (root) function: a helper inlined into a caller
 * acts on the caller's held set (caller_ok). */

struct mutex { long owner; long wait[4]; };
struct raw_spinlock { long raw; };
typedef struct raw_spinlock raw_spinlock_t;

void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);
void _raw_spin_unlock(raw_spinlock_t *lock);
void __prism_lock_entry(const void *lock, int spin);
void __prism_lock_exit(const void *lock);

static struct mutex gm = {0, {0, 0, 0, 0}};
static raw_spinlock_t gl = {0};

/* a helper that acquires and returns with the lock held: declared, so not a finding */
int take_declared_ok(void) {
    __prism_lock_exit(&gm);
    mutex_lock_nested(&gm, 0);
    return 0;
}

/* the same helper without the declaration: LOCK-HELD-ON-RETURN (a missing contract or a leak) */
int take_undeclared_bad(void) {
    mutex_lock_nested(&gm, 0);
    return 0;
}

/* declared as returning held, but released before the return: LOCK-EXIT-MISSING */
int take_exit_missing_bad(void) {
    __prism_lock_exit(&gm);
    mutex_lock_nested(&gm, 0);
    mutex_unlock(&gm);
    return 0;
}

/* the caller's lock (declared on entry) is released here: legal, no LOCK-UNHELD-UNLOCK */
int release_entry_ok(void) {
    __prism_lock_entry(&gm, 0);
    mutex_unlock(&gm);
    return 0;
}

/* the caller's lock is not released before the return: LOCK-HELD-ON-RETURN */
int entry_held_on_return_bad(void) {
    __prism_lock_entry(&gm, 0);
    return 0;
}

/* a lock held on entry and handed back to the caller: declared on entry and on exit */
int pass_through_ok(void) {
    __prism_lock_entry(&gm, 0);
    __prism_lock_exit(&gm);
    return 0;
}

/* the caller holds a spinlock on entry and the helper sleeps: SLEEP-IN-ATOMIC */
int entry_spin_sleep_bad(void) {
    __prism_lock_entry(&gl, 1);
    mutex_lock_nested(&gm, 0);
    mutex_unlock(&gm);
    _raw_spin_unlock(&gl);
    return 0;
}

/* a caller of the declared helper: the helper's lock is the caller's to release (inlined) */
int caller_ok(void) {
    take_declared_ok();
    mutex_unlock(&gm);
    return 0;
}
