/* Struct-field spinlock classes, consistent irq use (docs/KERNEL_CONCURRENCY.md section 16): the handler and the
 * process-context function both take dev->lock with irqsave, so there is no LOCK-CTX-MISMATCH. */

struct spinlock_s { long raw; };
struct dev { struct spinlock_s lock; long count; };
typedef int irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(int, void *);
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags, const char *name, void *dev);
void spin_lock_irqsave(struct spinlock_s *lock, unsigned long *flags);
void spin_unlock_irqrestore(struct spinlock_s *lock, unsigned long flags);

irqreturn_t irq_handler_ok(int irq, void *cookie) {
    struct dev *d = (struct dev *)cookie;
    unsigned long fl = 0;
    (void)irq;
    spin_lock_irqsave(&d->lock, &fl);
    d->count++;
    spin_unlock_irqrestore(&d->lock, fl);
    return 0;
}

void proc_irqsave(struct dev *d) {
    unsigned long fl = 0;
    spin_lock_irqsave(&d->lock, &fl);
    d->count++;
    spin_unlock_irqrestore(&d->lock, fl);
}

int setup_irq_false(struct dev *d) {
    return request_irq(7, irq_handler_ok, 0, "dev", d);
}
