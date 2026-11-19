/* Struct-field lock classes (docs/KERNEL_CONCURRENCY.md section 16). Run with --kernel-api linux.
 * dev->a and dev->b are two fields of struct dev: classes dev.#0 and dev.#1. f_ab takes a then b, f_ba takes b
 * then a: one LOCK-ORDER-INVERSION (dev.#0, dev.#1) on f_ba, witness f_ab. */

struct mutex { long owner; long wait[4]; };
struct dev { struct mutex a; struct mutex b; };
void mutex_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);

void f_ab(struct dev *d) {
    mutex_lock(&d->a);
    mutex_lock(&d->b);
    mutex_unlock(&d->b);
    mutex_unlock(&d->a);
}

void f_ba(struct dev *d) {
    mutex_lock(&d->b);
    mutex_lock(&d->a);
    mutex_unlock(&d->a);
    mutex_unlock(&d->b);
}
