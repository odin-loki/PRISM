/* Interrupt context (docs/KERNEL_CONCURRENCY.md section 15, W7c item 1). Run with --kernel-api linux.
 * irq_sleep_handler (request_irq) takes a mutex: SLEEP-IN-ATOMIC in hardirq. irq_gfp_handler (request_irq)
 * allocates with GFP_KERNEL: SLEEP-IN-ATOMIC in hardirq. irq_spin_handler takes dev_lock through touch_dev, a
 * callee at depth 1; proc_dev_plain takes dev_lock with plain spin_lock (interrupts enabled): LOCK-CTX-MISMATCH. */

struct mutex { long owner; long wait[4]; };
typedef struct raw_spinlock { long raw; } raw_spinlock_t;
typedef int irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(int, void *);

void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);
void _raw_spin_lock(raw_spinlock_t *lock);
void _raw_spin_unlock(raw_spinlock_t *lock);
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags, const char *name, void *dev);
void *kmalloc(unsigned long size, unsigned int flags);
void kfree(void *p);

#define GFP_KERNEL 0xcc0u

static raw_spinlock_t dev_lock = {0};
static struct mutex dev_mutex = {0, {0, 0, 0, 0}};

static void touch_dev(void) {
    _raw_spin_lock(&dev_lock);
    _raw_spin_unlock(&dev_lock);
}

irqreturn_t irq_spin_handler(int irq, void *dev) {
    (void)irq;
    (void)dev;
    touch_dev();
    return 0;
}

irqreturn_t irq_sleep_handler(int irq, void *dev) {
    (void)irq;
    (void)dev;
    mutex_lock_nested(&dev_mutex, 0);
    mutex_unlock(&dev_mutex);
    return 0;
}

irqreturn_t irq_gfp_handler(int irq, void *dev) {
    void *p;
    (void)irq;
    (void)dev;
    p = kmalloc(16, GFP_KERNEL);
    kfree(p);
    return 0;
}

/* the process context takes the hardirq lock with interrupts enabled */
int proc_dev_plain(void) {
    _raw_spin_lock(&dev_lock);
    _raw_spin_unlock(&dev_lock);
    return 0;
}

int probe_bad(void *dev) {
    request_irq(3, irq_spin_handler, 0, "spin", dev);
    request_irq(4, irq_sleep_handler, 0, "sleep", dev);
    request_irq(5, irq_gfp_handler, 0, "gfp", dev);
    return 0;
}
