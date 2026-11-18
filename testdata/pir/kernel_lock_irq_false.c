/* Interrupt context, consistent (docs/KERNEL_CONCURRENCY.md section 15, W7c item 1). Run with --kernel-api linux.
 * The handler and the process context both take aux_lock, the process side with interrupts disabled
 * (_raw_spin_lock_irqsave): no LOCK-CTX-MISMATCH. The handler allocates with GFP_ATOMIC: no SLEEP-IN-ATOMIC.
 * thread_sleep is the thread function of request_threaded_irq, which runs in process context: it may sleep. */

struct mutex { long owner; long wait[4]; };
typedef struct raw_spinlock { long raw; } raw_spinlock_t;
typedef int irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(int, void *);

void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);
void _raw_spin_lock(raw_spinlock_t *lock);
void _raw_spin_unlock(raw_spinlock_t *lock);
unsigned long _raw_spin_lock_irqsave(raw_spinlock_t *lock);
void _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags);
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags, const char *name, void *dev);
int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn, unsigned long flags,
                         const char *name, void *dev);
void *kmalloc(unsigned long size, unsigned int flags);
void kfree(void *p);

#define GFP_ATOMIC 0x820u

static raw_spinlock_t aux_lock = {0};
static struct mutex aux_mutex = {0, {0, 0, 0, 0}};

irqreturn_t irq_ok_handler(int irq, void *dev) {
    void *p;
    (void)irq;
    (void)dev;
    _raw_spin_lock(&aux_lock);
    _raw_spin_unlock(&aux_lock);
    p = kmalloc(16, GFP_ATOMIC);
    kfree(p);
    return 0;
}

int proc_aux_irqsave(void) {
    unsigned long flags = _raw_spin_lock_irqsave(&aux_lock);
    _raw_spin_unlock_irqrestore(&aux_lock, flags);
    return 0;
}

irqreturn_t thread_sleep(int irq, void *dev) {
    (void)irq;
    (void)dev;
    mutex_lock_nested(&aux_mutex, 0);
    mutex_unlock(&aux_mutex);
    return 0;
}

int probe_ok(void *dev) {
    request_irq(3, irq_ok_handler, 0, "ok", dev);
    request_threaded_irq(4, irq_ok_handler, thread_sleep, 0, "thr", dev);
    return 0;
}
