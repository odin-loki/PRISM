/* Linux kernel lock / PCI / runtime-PM / timer specs (docs/KERNEL_MODELS.md), W2 group c.
 *
 * Reference semantics (Linux 6.18):
 *   down_read(sem) (kernel/locking/rwsem.c:1534-1540): acquires the rw_semaphore for reading,
 *     sleeping until it is available; returns nothing.
 *   pci_write_config_dword(dev, where, val) (drivers/pci/access.c:607-613): returns
 *     PCIBIOS_DEVICE_NOT_FOUND if the device is disconnected, else the result of the bus
 *     config write (a PCIBIOS_* code, 0 on success). The model returns an arbitrary int.
 *   pm_runtime_enable(dev) (drivers/base/power/runtime.c:1566-1594): under dev->power.lock
 *     decrements the disable depth and updates the power state fields (it may warn); returns
 *     nothing.
 *   pci_disable_device(dev) (drivers/pci/pci.c:2227-2240): decrements dev->enable_cnt and,
 *     on reaching 0, disables the device and clears is_busmaster; returns nothing.
 *   timer_delete_sync(timer) (kernel/time/timer.c:1674-1677, include/linux/timer.h:166):
 *     deactivates the timer and waits for a running handler on another CPU; returns an int
 *     (0 if the timer was inactive, 1 if it was active). The model returns an arbitrary int.
 * Failure paths: pci_write_config_dword may fail (arbitrary int result); the others have none.
 * requires: sem, dev, timer point to valid kernel objects of the respective type (their first
 *   byte is checked).
 * Assumptions introduced (named in the verdict):
 *   - lock state, the hardware config space, the power-management and PCI enable state and the
 *     timer queue are not modelled: no effect on data memory (the real functions write fields
 *     of the object that are not modelled)
 *   - the results of pci_write_config_dword and timer_delete_sync are arbitrary (a superset)
 * Not specified (see NEEDS_REVIEW): skb_pull, of_device_get_match_data, simple_read_from_buffer,
 *   input_set_abs_params, release_firmware.
 */
#include "kernel_model.h"

extern "C" {

/* include/linux/rwsem.h: extern void down_read(struct rw_semaphore *sem) */
void down_read(void *sem) {
    ASSUME("linux-w2-down_read: acquires sem for reading, sleeping if needed (kernel/locking/rwsem.c:1534-1540, Linux 6.18); the lock state, the sleep and the lockdep/sleeping-context rules are not modelled, no effect on data memory; sem must be a valid struct rw_semaphore");
    __prism_read_range(sem, 1);
}

/* include/linux/pci.h: int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val) */
int pci_write_config_dword(const void *dev, int where, unsigned int val) {
    ASSUME("linux-w2-pci_write_config_dword: writes val to the config space at where, or returns PCIBIOS_DEVICE_NOT_FOUND / a bus error (drivers/pci/access.c:607-613, Linux 6.18); the config space and the bus are not modelled, no effect on data memory, the result is an arbitrary int (a superset of the real results); dev must be a valid struct pci_dev");
    (void)where; (void)val;
    __prism_read_range(dev, 1);
    return __VERIFIER_nondet_int();
}

/* include/linux/pm_runtime.h: extern void pm_runtime_enable(struct device *dev) */
void pm_runtime_enable(void *dev) {
    ASSUME("linux-w2-pm_runtime_enable: decrements dev->power.disable_depth and updates the runtime-PM state under dev->power.lock (drivers/base/power/runtime.c:1566-1594, Linux 6.18); the power fields, the lock and the warnings are not modelled: the real function writes dev->power fields that are not modelled, no other effect on data memory; dev must be a valid struct device");
    __prism_read_range(dev, 1);
}

/* include/linux/pci.h: void pci_disable_device(struct pci_dev *dev) */
void pci_disable_device(void *dev) {
    ASSUME("linux-w2-pci_disable_device: decrements dev->enable_cnt and disables the device when it reaches 0 (drivers/pci/pci.c:2227-2240, Linux 6.18); enable_cnt, is_busmaster, the device and the host bridge are not modelled: the real function writes pci_dev fields that are not modelled, no other effect on data memory; dev must be a valid struct pci_dev");
    __prism_read_range(dev, 1);
}

/* include/linux/timer.h: extern int timer_delete_sync(struct timer_list *timer) */
int timer_delete_sync(void *timer) {
    ASSUME("linux-w2-timer_delete_sync: deactivates the timer and waits for a running handler (kernel/time/timer.c:1674-1677, Linux 6.18); the timer queue, the wait and the handler are not modelled: the real function updates the timer entry, which is not modelled, no other effect on data memory; the result is an arbitrary int (a superset of the real 0/1); timer must be a valid struct timer_list");
    __prism_read_range(timer, 1);
    return __VERIFIER_nondet_int();
}

} /* extern "C" */
