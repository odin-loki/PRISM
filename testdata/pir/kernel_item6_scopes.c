/* PIR tasks: r1 FALSE triage item 6, the lock-order and interrupt-context pass with --pir-lock-scopes (run with
 * --kernel-api linux; tests/cpp/test_pir_triage_item6.cpp).
 *   - guard(mutex) scopes: class_mutex_constructor takes the lock, the cleanup class_mutex_destructor (called with
 *     the address of the guard variable at scope exit) releases it. scoped_a_then_b leaves lock_a's scope before it
 *     takes lock_b, so with b_then_a there is no A-B inversion; guarded_a_then_b takes lock_b inside lock_a's scope,
 *     so with b_then_a there is one.
 *   - readl_poll_timeout's sleep: `if (__sleep_us) usleep_range(...)` with sleep_us 0 is dead code in the hardirq
 *     handler irq_poll_zero; with sleep_us 1 (irq_poll_one) it sleeps in hardirq context. */

struct mutex { long owner; long wait[4]; };
void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);

typedef struct mutex *class_mutex_t;
static inline void class_mutex_destructor(class_mutex_t *p) {
    struct mutex *_T = *p;
    if (_T) mutex_unlock(_T);
}
static inline class_mutex_t class_mutex_constructor(struct mutex *_T) {
    class_mutex_t t = ({ mutex_lock_nested(_T, 0); _T; });
    return t;
}
#define guard_mutex(m) class_mutex_t __attribute__((__cleanup__(class_mutex_destructor))) __guard = class_mutex_constructor(m)

static struct mutex lock_a, lock_b;
int shared;

void scoped_a_then_b(void) {
    {
        guard_mutex(&lock_a);
        shared = 1;
    }
    mutex_lock_nested(&lock_b, 0);
    shared = 2;
    mutex_unlock(&lock_b);
}

void b_then_a(void) {
    mutex_lock_nested(&lock_b, 0);
    mutex_lock_nested(&lock_a, 0);
    shared = 3;
    mutex_unlock(&lock_a);
    mutex_unlock(&lock_b);
}

static struct mutex lock_c, lock_d;

void guarded_c_then_d(void) {
    guard_mutex(&lock_c);
    mutex_lock_nested(&lock_d, 0);
    shared = 4;
    mutex_unlock(&lock_d);
}

void d_then_c(void) {
    mutex_lock_nested(&lock_d, 0);
    mutex_lock_nested(&lock_c, 0);
    shared = 5;
    mutex_unlock(&lock_c);
    mutex_unlock(&lock_d);
}

/* interrupt context */
typedef int (*irq_handler_t)(int, void *);
int devm_request_irq(void *dev, unsigned int irq, irq_handler_t handler, unsigned long flags, const char *name, void *d);
void usleep_range(unsigned long min, unsigned long max);
volatile unsigned int hw_status;

#define poll_status(sleep_us)                                                    \
    ({                                                                           \
        unsigned long __sleep_us = (sleep_us);                                   \
        int __n = 0;                                                             \
        for (;;) {                                                               \
            if ((hw_status & 1) || __n++ > 3) break;                             \
            if (__sleep_us) usleep_range((__sleep_us >> 2) + 1, __sleep_us);     \
        }                                                                        \
        (int)(hw_status & 1);                                                    \
    })

static int irq_poll_zero(int irq, void *d) {
    (void)irq; (void)d;
    return poll_status(0);
}
static int irq_poll_one(int irq, void *d) {
    (void)irq; (void)d;
    return poll_status(1);
}
int probe_irqs(void *dev) {
    devm_request_irq(dev, 1, irq_poll_zero, 0, "zero", 0);
    devm_request_irq(dev, 2, irq_poll_one, 0, "one", 0);
    return 0;
}
