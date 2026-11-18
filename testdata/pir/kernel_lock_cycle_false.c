/* Lock order, no cycle (docs/KERNEL_CONCURRENCY.md section 14, W7c item 2). Run with --kernel-api linux.
 * A before B, B before C, A before C: a DAG, no cycle and no inversion. */

struct mutex { long owner; long wait[4]; };

void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);

static struct mutex lock_a = {0, {0, 0, 0, 0}};
static struct mutex lock_b = {0, {0, 0, 0, 0}};
static struct mutex lock_c = {0, {0, 0, 0, 0}};

void g1(void) {
    mutex_lock_nested(&lock_a, 0);
    mutex_lock_nested(&lock_b, 0);
    mutex_unlock(&lock_b);
    mutex_unlock(&lock_a);
}

void g2(void) {
    mutex_lock_nested(&lock_b, 0);
    mutex_lock_nested(&lock_c, 0);
    mutex_unlock(&lock_c);
    mutex_unlock(&lock_b);
}

void g3(void) {
    mutex_lock_nested(&lock_a, 0);
    mutex_lock_nested(&lock_c, 0);
    mutex_unlock(&lock_c);
    mutex_unlock(&lock_a);
}
