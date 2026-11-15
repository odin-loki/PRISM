/* PIR tasks: lock balance, double lock, unlock of an unheld lock and sleeping in
 * atomic context (docs/KERNEL_CONCURRENCY.md, phase A). Run with `--kernel-api linux`.
 * Each *_ok function is PROVED-ASSUMING (linux-lock); each *_bad function is FAILED
 * with the class in its name (LOCK-DOUBLE, LOCK-UNHELD-UNLOCK, CTX-SLEEP-ATOMIC).
 * The kernel declarations are the kernel's own, as in kernel_linux.c. */

struct mutex { long owner; long wait[4]; };
struct raw_spinlock { long raw; };
typedef struct raw_spinlock raw_spinlock_t;

void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);
void _raw_spin_lock(raw_spinlock_t *lock);
void _raw_spin_unlock(raw_spinlock_t *lock);
void __might_sleep(const char *file, int line);

/* LOCK-DOUBLE: a second acquire of a lock this function holds (self-deadlock) */
int lock_double_bad(void) {
    struct mutex m = {0, {0, 0, 0, 0}};
    mutex_lock_nested(&m, 0);
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    mutex_unlock(&m);
    return 0;
}

/* the same lock taken again after its release is not a double lock */
int lock_relock_ok(void) {
    struct mutex m = {0, {0, 0, 0, 0}};
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    return 0;
}

/* a spinlock taken twice is a double lock too */
int spin_double_bad(void) {
    raw_spinlock_t l = {0};
    _raw_spin_lock(&l);
    _raw_spin_lock(&l);
    _raw_spin_unlock(&l);
    _raw_spin_unlock(&l);
    return 0;
}

/* two different locks nested is not a double lock */
int spin_nested_ok(void) {
    raw_spinlock_t a = {0}, b = {0};
    _raw_spin_lock(&a);
    _raw_spin_lock(&b);
    _raw_spin_unlock(&b);
    _raw_spin_unlock(&a);
    return 0;
}

/* LOCK-UNHELD-UNLOCK: release of a lock this function never took */
int unlock_unheld_bad(void) {
    struct mutex m = {0, {0, 0, 0, 0}};
    mutex_unlock(&m);
    return 0;
}

/* CTX-SLEEP-ATOMIC: a sleeping lock taken while a spinlock is held */
int sleep_in_spin_bad(void) {
    raw_spinlock_t l = {0};
    struct mutex m = {0, {0, 0, 0, 0}};
    _raw_spin_lock(&l);
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    _raw_spin_unlock(&l);
    return 0;
}

/* the sleeping lock taken after the spinlock is released is legal */
int sleep_after_spin_ok(void) {
    raw_spinlock_t l = {0};
    struct mutex m = {0, {0, 0, 0, 0}};
    _raw_spin_lock(&l);
    _raw_spin_unlock(&l);
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    return 0;
}

/* CTX-SLEEP-ATOMIC through might_sleep() inside a spinlock */
int might_sleep_in_spin_bad(void) {
    raw_spinlock_t l = {0};
    _raw_spin_lock(&l);
    __might_sleep("kernel_lock_ctx.c", 1);
    _raw_spin_unlock(&l);
    return 0;
}

/* might_sleep() with no spinlock held is legal */
int might_sleep_ok(void) {
    __might_sleep("kernel_lock_ctx.c", 1);
    return 0;
}
