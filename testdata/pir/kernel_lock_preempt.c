/* Preempt and bh depth, sleeping in atomic context (docs/KERNEL_CONCURRENCY.md section 5,
 * W7r step 2). Run with `--kernel-api linux`. Each *_ok function is PROVED-ASSUMING;
 * each *_bad function is FAILED with the class in its name (PREEMPT-UNBALANCED or
 * SLEEP-IN-ATOMIC). Sleeping callees: mutex_lock_nested, __kmalloc_noprof with
 * __GFP_DIRECT_RECLAIM (GFP_KERNEL 0xcc0), not with GFP_ATOMIC (0x820). */

struct mutex { long owner; long wait[4]; };
typedef struct raw_spinlock { long raw; } raw_spinlock_t;

void preempt_count_add(int val);
void preempt_count_sub(int val);
void __local_bh_disable_ip(unsigned long ip, unsigned int cnt);
void __local_bh_enable_ip(unsigned long ip, unsigned int cnt);
void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);
void _raw_spin_lock(raw_spinlock_t *lock);
void _raw_spin_unlock(raw_spinlock_t *lock);
void *__kmalloc_noprof(unsigned long size, unsigned int flags);
void kfree(const void *p);

/* preempt_disable, then a sleeping lock: SLEEP-IN-ATOMIC */
int preempt_sleep_bad(void) {
    struct mutex m = {0, {0, 0, 0, 0}};
    preempt_count_add(1);
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    preempt_count_sub(1);
    return 0;
}

/* the sleeping lock after preempt_enable is legal */
int preempt_sleep_ok(void) {
    struct mutex m = {0, {0, 0, 0, 0}};
    preempt_count_add(1);
    preempt_count_sub(1);
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    return 0;
}

/* preempt_enable without a matching preempt_disable: PREEMPT-UNBALANCED */
int preempt_unbalanced_bad(void) {
    preempt_count_sub(1);
    return 0;
}

/* preempt_disable never undone before the return: PREEMPT-UNBALANCED */
int preempt_held_on_return_bad(void) {
    preempt_count_add(1);
    return 0;
}

/* bottom halves disabled, then a sleeping lock: SLEEP-IN-ATOMIC */
int bh_sleep_bad(void) {
    struct mutex m = {0, {0, 0, 0, 0}};
    __local_bh_disable_ip(0, 1);
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    __local_bh_enable_ip(0, 1);
    return 0;
}

/* bottom halves disabled and enabled again, then a sleeping lock: legal */
int bh_sleep_ok(void) {
    struct mutex m = {0, {0, 0, 0, 0}};
    __local_bh_disable_ip(0, 1);
    __local_bh_enable_ip(0, 1);
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    return 0;
}

/* a GFP_KERNEL allocation (may reclaim, may sleep) inside a spinlock: SLEEP-IN-ATOMIC */
int kmalloc_in_spin_bad(void) {
    raw_spinlock_t l = {0};
    _raw_spin_lock(&l);
    void *p = __kmalloc_noprof(16, 0xcc0u);
    kfree(p);
    _raw_spin_unlock(&l);
    return 0;
}

/* a GFP_ATOMIC allocation (no direct reclaim) inside a spinlock: legal */
int kmalloc_atomic_ok(void) {
    raw_spinlock_t l = {0};
    _raw_spin_lock(&l);
    void *p = __kmalloc_noprof(16, 0x820u);
    kfree(p);
    _raw_spin_unlock(&l);
    return 0;
}

/* a GFP_KERNEL allocation with no lock anywhere: legal (no atomic state) */
int kmalloc_kernel_ok(void) {
    void *p = __kmalloc_noprof(16, 0xcc0u);
    kfree(p);
    return 0;
}
