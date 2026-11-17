/* RCU read-side nesting (docs/KERNEL_CONCURRENCY.md section 3, W7r step 3). Run with
 * `--kernel-api linux`. Each *_ok function is PROVED-ASSUMING (linux-rcu); each *_bad
 * function is FAILED with the class in its name (RCU-UNBALANCED, RCU-HELD-ON-RETURN, RCU-SLEEP). */

struct mutex { long owner; long wait[4]; };

void __rcu_read_lock(void);
void __rcu_read_unlock(void);
void synchronize_rcu(void);
void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);

/* a balanced read-side section: legal */
int rcu_balance_ok(void) {
    __rcu_read_lock();
    __rcu_read_unlock();
    return 0;
}

/* rcu_read_unlock without a lock: RCU-UNBALANCED */
int rcu_unbalanced_bad(void) {
    __rcu_read_unlock();
    return 0;
}

/* the error path returns inside the read-side section: RCU-HELD-ON-RETURN */
int rcu_held_on_return_bad(int err) {
    __rcu_read_lock();
    if (err) return -1;
    __rcu_read_unlock();
    return 0;
}

/* a sleeping lock inside a read-side section: RCU-SLEEP */
int rcu_sleep_bad(void) {
    struct mutex m = {0, {0, 0, 0, 0}};
    __rcu_read_lock();
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    __rcu_read_unlock();
    return 0;
}

/* a sleeping lock after the read-side section: legal */
int rcu_sleep_ok(void) {
    struct mutex m = {0, {0, 0, 0, 0}};
    __rcu_read_lock();
    __rcu_read_unlock();
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    return 0;
}

/* synchronize_rcu inside a read-side section (it waits for the section to end): RCU-SLEEP */
int rcu_sync_bad(void) {
    __rcu_read_lock();
    synchronize_rcu();
    __rcu_read_unlock();
    return 0;
}

/* synchronize_rcu outside any read-side section: legal */
int rcu_sync_ok(void) {
    synchronize_rcu();
    return 0;
}
