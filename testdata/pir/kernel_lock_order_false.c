/* Lock order, consistent (docs/KERNEL_CONCURRENCY.md section 14, W7b step 2). Run with
 * --kernel-api linux. Every path takes lock_a before lock_b, directly or through a callee:
 * no LOCK-ORDER-INVERSION. */

struct mutex { long owner; long wait[4]; };

void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);

static struct mutex lock_a = {0, {0, 0, 0, 0}};
static struct mutex lock_b = {0, {0, 0, 0, 0}};

static void take_b_locked(void) { mutex_lock_nested(&lock_b, 0); }
static void release_b(void) { mutex_unlock(&lock_b); }

void ab_path(void) {
    mutex_lock_nested(&lock_a, 0);
    mutex_lock_nested(&lock_b, 0);
    mutex_unlock(&lock_b);
    mutex_unlock(&lock_a);
}

void ab_via_callee(void) {
    mutex_lock_nested(&lock_a, 0);
    take_b_locked();
    release_b();
    mutex_unlock(&lock_a);
}

void b_only_ok(void) {
    mutex_lock_nested(&lock_b, 0);
    mutex_unlock(&lock_b);
}
