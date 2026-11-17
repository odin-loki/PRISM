/* Lock order, three classes in a cycle (docs/KERNEL_CONCURRENCY.md section 14, W7c item 2). Run with
 * --kernel-api linux. f1 takes lock_a then lock_b, f2 takes lock_b then lock_c, f3 takes lock_c then lock_a:
 * no pair is taken in both orders, so only the cycle is reported (one LOCK-ORDER-INVERSION, on f3). */

struct mutex { long owner; long wait[4]; };

void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);

static struct mutex lock_a = {0, {0, 0, 0, 0}};
static struct mutex lock_b = {0, {0, 0, 0, 0}};
static struct mutex lock_c = {0, {0, 0, 0, 0}};

void f1(void) {
    mutex_lock_nested(&lock_a, 0);
    mutex_lock_nested(&lock_b, 0);
    mutex_unlock(&lock_b);
    mutex_unlock(&lock_a);
}

void f2(void) {
    mutex_lock_nested(&lock_b, 0);
    mutex_lock_nested(&lock_c, 0);
    mutex_unlock(&lock_c);
    mutex_unlock(&lock_b);
}

void f3(void) {
    mutex_lock_nested(&lock_c, 0);
    mutex_lock_nested(&lock_a, 0);
    mutex_unlock(&lock_a);
    mutex_unlock(&lock_c);
}
