/* --lock-contracts (docs/KERNEL_CONCURRENCY.md section 14): contracts for code with no
 * markers, from the TSV testdata/pir/kernel_lock_table.tsv. Run with --kernel-api linux.
 * Without the table the acquire half (nvm_acquire_ok) is LOCK-HELD-ON-RETURN and the release
 * half (nvm_release_ok) is LOCK-UNHELD-UNLOCK: the model artefacts of the e1000e pilot. With
 * the table they are PROVED-ASSUMING (lock-contract-table). wrong_contract_bad declares an
 * acquire but releases before the return: LOCK-EXIT-MISSING. spin_entry_sleep_bad releases a
 * spinlock that the table says the caller holds: the mutex call inside is SLEEP-IN-ATOMIC. */

struct mutex { long owner; long wait[4]; };
typedef struct raw_spinlock { long raw; } raw_spinlock_t;

void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);
void _raw_spin_unlock(raw_spinlock_t *lock);

static struct mutex nvm_mutex = {0, {0, 0, 0, 0}};
static raw_spinlock_t gl = {0};

int nvm_acquire_ok(void) {
    mutex_lock_nested(&nvm_mutex, 0);
    return 0;
}

void nvm_release_ok(void) {
    mutex_unlock(&nvm_mutex);
}

int wrong_contract_bad(void) {
    mutex_lock_nested(&nvm_mutex, 0);
    mutex_unlock(&nvm_mutex);
    return 0;
}

void spin_entry_sleep_bad(void) {
    mutex_lock_nested(&nvm_mutex, 0);
    mutex_unlock(&nvm_mutex);
    _raw_spin_unlock(&gl);
}
