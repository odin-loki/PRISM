/* Lock order (docs/KERNEL_CONCURRENCY.md section 14, W7b step 2). Run with --kernel-api linux.
 * ab_path takes lock_a then lock_b; ba_path takes lock_b then lock_a, and ba_via_callee takes
 * lock_a in a callee while holding lock_b. The unit has a LOCK-ORDER-INVERSION for the two classes. */

struct mutex { long owner; long wait[4]; };

void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);

static struct mutex lock_a = {0, {0, 0, 0, 0}};
static struct mutex lock_b = {0, {0, 0, 0, 0}};

static void take_a_locked(void) { mutex_lock_nested(&lock_a, 0); }
static void release_a(void) { mutex_unlock(&lock_a); }

void ab_path(void) {
    mutex_lock_nested(&lock_a, 0);
    mutex_lock_nested(&lock_b, 0);
    mutex_unlock(&lock_b);
    mutex_unlock(&lock_a);
}

void ba_path(void) {
    mutex_lock_nested(&lock_b, 0);
    mutex_lock_nested(&lock_a, 0);
    mutex_unlock(&lock_a);
    mutex_unlock(&lock_b);
}

void ba_via_callee(void) {
    mutex_lock_nested(&lock_b, 0);
    take_a_locked();
    release_a();
    mutex_unlock(&lock_b);
}
