/* Linux kernel timer, PCI config, mutex, IRQ, rwsem and sleep specs (docs/KERNEL_MODELS.md), W2 group b.
 *
 * Reference semantics (Linux 6.18):
 *   mod_timer(timer, expires) (kernel/time/timer.c:1193-1196, = __mod_timer(timer, expires, 0)):
 *     (re)arms the timer; returns int (0: timer was inactive, 1: it was pending). The model
 *     returns an arbitrary int (a superset of the real results). The timer fields and the
 *     timer wheel are not modelled.
 *   pci_read_config_dword(dev, where, val) (drivers/pci/access.c:580-588): reads a 32-bit
 *     config register into *val; returns a PCIBIOS_* code (0 on success). On a disconnected
 *     device *val is set to the error response and an error is returned; on some error paths
 *     (e.g. unaligned register) *val is not written. The model chooses arbitrarily whether
 *     *val is written (with an arbitrary u32) and returns an arbitrary int.
 *   mutex_destroy(lock) (kernel/locking/mutex-debug.c:113-117): debug-only check that the
 *     mutex is unlocked and clears lock->magic; returns nothing.
 *   platform_get_irq(dev, num) (drivers/base/platform.c:260-270): returns an IRQ number or a
 *     negative errno; the model returns an arbitrary int.
 *   down_write(sem) (kernel/locking/rwsem.c:1587-1592): acquires the semaphore for writing,
 *     sleeping if needed; returns nothing.
 *   schedule_timeout(timeout) (kernel/time/sleep_timeout.c:61-109): sleeps; returns the
 *     remaining time, which is never negative (the final statement is
 *     "return timeout < 0 ? 0 : timeout"). The model returns an arbitrary non-negative long.
 * Failure paths: mod_timer, platform_get_irq, pci_read_config_dword may return any int.
 * requires: timer / dev (pci_dev) / lock / dev (platform_device) / sem point to valid kernel
 *   objects (one byte of each is read); val points to 4 writable bytes.
 * Assumptions introduced (named in the verdict):
 *   - the timer wheel, config space, lock state, IRQ resources and the sleep are not modelled
 *   - results are arbitrary (superset of the real results), except schedule_timeout >= 0
 */
#include "kernel_model.h"

extern "C" {

/* include/linux/timer.h: extern int mod_timer(struct timer_list *timer, unsigned long expires) */
int mod_timer(void *timer, unsigned long expires) {
    ASSUME("linux-w2-mod_timer: re-arms the timer (kernel/time/timer.c:1193-1196, Linux 6.18); the timer wheel and timer fields are not modelled, no effect on data memory, the result is an arbitrary int (a superset of the real 0/1); timer must be a valid struct timer_list");
    (void)expires;
    __prism_read_range(timer, 1);
    return __VERIFIER_nondet_int();
}

/* include/linux/pci.h: int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val) */
int pci_read_config_dword(const void *dev, int where, unsigned int *val) {
    (void)where;
    __prism_read_range(dev, 1);
    if (CHOICE()) {
        ASSUME("linux-w2-pci_read_config_dword: reads a config dword (drivers/pci/access.c:580-588, Linux 6.18); config space is not modelled: *val is either written with an arbitrary u32 or left untouched (error paths), the result is an arbitrary int; dev must be a valid struct pci_dev, val 4 writable bytes");
        unsigned int tmp = (unsigned int)__VERIFIER_nondet_int();
        __prism_memcpy((void *)val, &tmp, 4, 0);
    } else {
        ASSUME("linux-w2-pci_read_config_dword: reads a config dword (drivers/pci/access.c:580-588, Linux 6.18); config space is not modelled: *val is either written with an arbitrary u32 or left untouched (error paths), the result is an arbitrary int; dev must be a valid struct pci_dev, val 4 writable bytes");
    }
    return __VERIFIER_nondet_int();
}

/* include/linux/mutex.h: extern void mutex_destroy(struct mutex *lock) */
void mutex_destroy(void *lock) {
    ASSUME("linux-w2-mutex_destroy: debug check that the mutex is unlocked and clearing of lock->magic (kernel/locking/mutex-debug.c:113-117, Linux 6.18); the lock state and the magic write are not modelled, no effect on data memory; lock must be a valid struct mutex");
    __prism_read_range(lock, 1);
}

/* include/linux/platform_device.h: extern int platform_get_irq(struct platform_device *, unsigned int) */
int platform_get_irq(void *dev, unsigned int num) {
    ASSUME("linux-w2-platform_get_irq: looks up IRQ resource num (drivers/base/platform.c:260-270, Linux 6.18); resources are not modelled, no effect on data memory, the result is an arbitrary int (an IRQ number or a negative errno; a superset of the real results); dev must be a valid struct platform_device");
    (void)num;
    __prism_read_range(dev, 1);
    return __VERIFIER_nondet_int();
}

/* include/linux/rwsem.h: extern void down_write(struct rw_semaphore *sem) */
void down_write(void *sem) {
    ASSUME("linux-w2-down_write: acquires the rw_semaphore for writing, possibly sleeping (kernel/locking/rwsem.c:1587-1592, Linux 6.18); the lock state, the sleep and lockdep are not modelled, no effect on data memory; sem must be a valid struct rw_semaphore");
    __prism_read_range(sem, 1);
}

/* include/linux/sched.h: extern long schedule_timeout(long timeout) */
long schedule_timeout(long timeout) {
    ASSUME("linux-w2-schedule_timeout: sleeps and returns the remaining time (kernel/time/sleep_timeout.c:61-109, Linux 6.18); the sleep and the timer are not modelled, no effect on data memory, the result is an arbitrary non-negative long (the real result is never negative; a superset otherwise)");
    (void)timeout;
    unsigned long hi = (unsigned long)(unsigned int)__VERIFIER_nondet_int();
    unsigned long lo = (unsigned long)(unsigned int)__VERIFIER_nondet_int();
    unsigned long v = ((hi << 32) | lo) & 0x7fffffffffffffffUL;
    return (long)v;
}

} /* extern "C" */
