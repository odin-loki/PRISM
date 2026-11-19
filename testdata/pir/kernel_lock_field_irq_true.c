/* Struct-field spinlock classes in interrupt context (docs/KERNEL_CONCURRENCY.md section 16). Run with --kernel-api linux.
 * irq_handler (request_irq) takes dev->lock (class dev.#0) with spin_lock_irqsave through touch_lock, a callee at
 * depth 1. proc_plain takes the same field with plain spin_lock (interrupts enabled): LOCK-CTX-MISMATCH. */

struct spinlock_s { long raw; };
struct dev { struct spinlock_s lock; long count; };
typedef int irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(int, void *);
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags, const char *name, void *dev);
void spin_lock(struct spinlock_s *lock);
void spin_unlock(struct spinlock_s *lock);
void spin_lock_irqsave(struct spinlock_s *lock, unsigned long *flags);
void spin_unlock_irqrestore(struct spinlock_s *lock, unsigned long flags);

static void touch_lock(struct dev *d) {
    unsigned long fl = 0;
    spin_lock_irqsave(&d->lock, &fl);
    d->count++;
    spin_unlock_irqrestore(&d->lock, fl);
}

irqreturn_t irq_handler(int irq, void *cookie) {
    (void)irq;
    touch_lock((struct dev *)cookie);
    return 0;
}

void proc_plain(struct dev *d) {
    spin_lock(&d->lock);
    d->count++;
    spin_unlock(&d->lock);
}

int setup_irq_true(struct dev *d) {
    return request_irq(7, irq_handler, 0, "dev", d);
}
