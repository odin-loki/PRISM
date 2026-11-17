/* PIR tasks: Linux kernel API specs, W2 group b (docs/KERNEL_MODELS.md). Run with
 * `--kernel-api linux`. Opaque kernel objects are stood for by local char arrays. */

int mod_timer(void *timer, unsigned long expires);
int pci_read_config_dword(const void *dev, int where, unsigned int *val);
void mutex_destroy(void *lock);
int platform_get_irq(void *dev, unsigned int num);
void down_write(void *sem);
long schedule_timeout(long timeout);

/* ---- mod_timer ---- */
int mod_timer_ok(void) {
    char timer[64]; /* stands for a struct timer_list */
    int r = mod_timer(timer, 100);
    (void)r; /* any result is a normal return */
    return 0;
}
int mod_timer_bad(void) { /* the result is arbitrary: may be 0 */
    char timer[64];
    return 100 / mod_timer(timer, 100);
}

/* ---- pci_read_config_dword ---- */
int pci_read_config_dword_ok(void) {
    char dev[64]; /* stands for a struct pci_dev */
    unsigned int v = 0;
    int r = pci_read_config_dword(dev, 0, &v);
    (void)r;
    return (int)(v & 1); /* any value is acceptable */
}
int pci_read_config_dword_bad(void) { /* unchecked result: *val may stay 0 */
    char dev[64];
    unsigned int v = 0;
    (void)pci_read_config_dword(dev, 0, &v);
    return 100 / (int)v;
}

/* ---- mutex_destroy ---- */
int mutex_destroy_ok(void) {
    char lock[32]; /* stands for a struct mutex */
    mutex_destroy(lock);
    return 0;
}
int mutex_destroy_bad(void) { /* lock is NULL */
    mutex_destroy((void *)0);
    return 0;
}

/* ---- platform_get_irq ---- */
int platform_get_irq_ok(void) {
    char pdev[64]; /* stands for a struct platform_device */
    int irq = platform_get_irq(pdev, 0);
    if (irq < 0) return irq;
    return 0;
}
int platform_get_irq_bad(void) { /* the result is not checked: may be 0 */
    char pdev[64];
    return 100 / platform_get_irq(pdev, 0);
}

/* ---- down_write ---- */
int down_write_ok(void) {
    char sem[32]; /* stands for a struct rw_semaphore */
    down_write(sem);
    return 0;
}
int down_write_bad(void) { /* sem is NULL */
    down_write((void *)0);
    return 0;
}

/* ---- schedule_timeout ---- */
int schedule_timeout_ok(void) {
    long r = schedule_timeout(10);
    return r < 0 ? -1 : 0; /* never negative */
}
int schedule_timeout_bad(void) { /* remaining time may be 0 */
    long r = schedule_timeout(10);
    return (int)(100 / r);
}
