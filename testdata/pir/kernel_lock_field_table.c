/* Field contract rows (docs/KERNEL_CONCURRENCY.md section 16): take_a and release_a are contract-only callees with
 * lock field:dev.0 (class dev.#0). f_ab_table takes a (through take_a) then b; f_ba_table takes b and then a while
 * holding b: one LOCK-ORDER-INVERSION with the table (kernel_lock_field_table.tsv), none without it. */

struct mutex { long owner; long wait[4]; };
struct dev { struct mutex a; struct mutex b; };
void mutex_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);
void take_a(void);
void release_a(void);

void f_ab_table(struct dev *d) {
    take_a();
    mutex_lock(&d->b);
    mutex_unlock(&d->b);
    release_a();
}

void f_ba_table(struct dev *d) {
    mutex_lock(&d->b);
    take_a();
    release_a();
    mutex_unlock(&d->b);
}
