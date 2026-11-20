/* PIR tasks: Linux kernel API specs, W2 group c (docs/KERNEL_MODELS.md). Run with
 * `--kernel-api linux`. Opaque kernel objects are stood for by local char objects. */

void down_read(void *sem);
int pci_write_config_dword(const void *dev, int where, unsigned int val);
void pm_runtime_enable(void *dev);
void pci_disable_device(void *dev);
int timer_delete_sync(void *timer);

int down_read_ok(void) {
    char sem; /* stands for a struct rw_semaphore */
    down_read(&sem);
    return 0;
}
int down_read_bad(void) { /* sem is NULL */
    down_read((void *)0);
    return 0;
}

int pci_write_config_dword_ok(void) {
    char dev; /* stands for a struct pci_dev */
    int r = pci_write_config_dword(&dev, 4, 0x1234u);
    (void)r; /* any PCIBIOS code is a normal return */
    return 0;
}
int pci_write_config_dword_bad(void) { /* the result is not checked: it may be 0 */
    char dev;
    int r = pci_write_config_dword(&dev, 4, 0x1234u);
    return 100 / r;
}

int pm_runtime_enable_ok(void) {
    char dev; /* stands for a struct device */
    pm_runtime_enable(&dev);
    return 0;
}
int pm_runtime_enable_bad(void) { /* dev is NULL */
    pm_runtime_enable((void *)0);
    return 0;
}

int pci_disable_device_ok(void) {
    char dev; /* stands for a struct pci_dev */
    pci_disable_device(&dev);
    return 0;
}
int pci_disable_device_bad(void) { /* dev is NULL */
    pci_disable_device((void *)0);
    return 0;
}

int timer_delete_sync_ok(void) {
    char timer; /* stands for a struct timer_list */
    int r = timer_delete_sync(&timer);
    (void)r; /* active or not: both are normal returns */
    return 0;
}
int timer_delete_sync_bad(void) { /* the result may be 0 (timer was inactive) */
    char timer;
    int r = timer_delete_sync(&timer);
    return 100 / r;
}
