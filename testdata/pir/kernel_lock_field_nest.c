/* Same-class nesting (docs/KERNEL_CONCURRENCY.md section 16): a->lock and b->lock are both adapter.#0, so
 * nest_ab and nest_ba (opposite nesting of two instances of one class) are NOT an inversion. */

struct mutex { long owner; long wait[4]; };
struct adapter { struct mutex lock; long n; };
void mutex_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);

void nest_ab(struct adapter *a, struct adapter *b) {
    mutex_lock(&a->lock);
    mutex_lock(&b->lock);
    mutex_unlock(&b->lock);
    mutex_unlock(&a->lock);
}

void nest_ba(struct adapter *a, struct adapter *b) {
    mutex_lock(&b->lock);
    mutex_lock(&a->lock);
    mutex_unlock(&a->lock);
    mutex_unlock(&b->lock);
}
