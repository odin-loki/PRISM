/* Struct-field lock classes, consistent order (docs/KERNEL_CONCURRENCY.md section 16): both functions take
 * dev->a then dev->b, so there is no LOCK-ORDER-INVERSION. */

struct mutex { long owner; long wait[4]; };
struct dev { struct mutex a; struct mutex b; };
void mutex_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);

void g_one(struct dev *d) {
    mutex_lock(&d->a);
    mutex_lock(&d->b);
    mutex_unlock(&d->b);
    mutex_unlock(&d->a);
}

void g_two(struct dev *d) {
    mutex_lock(&d->a);
    mutex_unlock(&d->a);
    mutex_lock(&d->b);
    mutex_unlock(&d->b);
}
