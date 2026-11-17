/* Linux kernel W2 group d specs (docs/KERNEL_MODELS.md): rtnl_lock, reset_control_assert,
 * jiffies_to_msecs, pci_read_config_word, is_acpi_device_node, netif_carrier_off.
 *
 * Reference semantics (Linux 6.18):
 *   rtnl_lock(void) (net/core/rtnetlink.c:78-81): mutex_lock(&rtnl_mutex); returns nothing.
 *   int reset_control_assert(struct reset_control *rstc) (include/linux/reset.h:75): the quoted
 *     definition (reset.h:122-125) is the stub used without a reset controller and returns 0; the
 *     real one (drivers/reset/core.c) is not quoted and may return a negative errno. The model
 *     returns an arbitrary int. rstc may be NULL or an error pointer in the real function, so it
 *     is not dereferenced by the model.
 *   unsigned int jiffies_to_msecs(unsigned long j) (kernel/time/time.c:377-391): converts jiffies
 *     to milliseconds; the formula depends on HZ, which is not known: the model returns an
 *     arbitrary unsigned int.
 *   int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
 *     (drivers/pci/access.c:570-577): on a disconnected device sets *val to the error response
 *     (0xFFFF) and returns PCIBIOS_DEVICE_NOT_FOUND; otherwise reads the config word through the
 *     bus ops; returns 0 or a PCIBIOS_* error code. *val may be left untouched on some errors.
 *   bool is_acpi_device_node(const struct fwnode_handle *fwnode) (include/acpi/acpi_bus.h:515;
 *     the quoted include/linux/acpi.h:844-847 is the stub returning false): the model returns an
 *     arbitrary bool; fwnode may be NULL, so it is not dereferenced.
 *   netif_carrier_off(struct net_device *dev) (net/sched/sch_generic.c:623-631): sets the
 *     NOCARRIER bit in dev->state, and if the bit was clear and the device is registered, bumps
 *     carrier_down_count and fires a linkwatch event; returns nothing.
 * Failure paths: reset_control_assert and pci_read_config_word may fail (arbitrary int result).
 * requires: dev of pci_read_config_word points to a valid struct pci_dev (one byte is checked);
 *   val points to 2 writable bytes; dev of netif_carrier_off points to a valid struct net_device
 *   (one byte is checked).
 * Assumptions introduced (named in the verdict):
 *   - the lock state of rtnl_mutex, sleeping-context rules and lock ordering are not modelled
 *   - results of reset_control_assert, jiffies_to_msecs, is_acpi_device_node and
 *     pci_read_config_word are arbitrary (a superset of the real results)
 *   - *val of pci_read_config_word is set to an arbitrary u16 on every path (a superset of
 *     0xFFFF / the register value / untouched)
 *   - netif_carrier_off's changes to dev->state, carrier_down_count and linkwatch are not modelled
 * Not specified (see NEEDS_REVIEW): xa_find, xa_find_after, iput.
 */
#include "kernel_model.h"

extern "C" {

/* include/linux/rtnetlink.h: extern void rtnl_lock(void) */
void rtnl_lock(void) {
    ASSUME("linux-w2-rtnl_lock: takes the rtnl mutex (net/core/rtnetlink.c:78-81, Linux 6.18); the lock state, lock ordering and sleeping-context rules are not modelled, no effect on data memory");
}

/* include/linux/reset.h: int reset_control_assert(struct reset_control *rstc) */
int reset_control_assert(void *rstc) {
    (void)rstc;
    ASSUME("linux-w2-reset_control_assert: asserts the reset line (include/linux/reset.h:75; the quoted stub at reset.h:122-125 returns 0, the real drivers/reset/core.c version may return a negative errno); the reset state is not modelled, no effect on data memory, the result is an arbitrary int (a superset of the real results); rstc is not dereferenced");
    return __VERIFIER_nondet_int();
}

/* include/linux/jiffies.h: extern unsigned int jiffies_to_msecs(const unsigned long j) */
unsigned int jiffies_to_msecs(unsigned long j) {
    (void)j;
    ASSUME("linux-w2-jiffies_to_msecs: converts jiffies to milliseconds (kernel/time/time.c:377-391, Linux 6.18); HZ is not known, so the result is an arbitrary unsigned int (a superset of the real results)");
    return (unsigned int)__VERIFIER_nondet_int();
}

/* include/linux/pci.h: int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val) */
int pci_read_config_word(const void *dev, int where, unsigned short *val) {
    (void)where;
    ASSUME("linux-w2-pci_read_config_word: reads a 16-bit PCI config word into *val (drivers/pci/access.c:570-577, Linux 6.18); the device, bus ops and config space are not modelled: *val is set to an arbitrary u16 (a superset of the register value, the 0xFFFF error response and untouched) and the result is an arbitrary int (0 or a PCIBIOS_* error); dev must be a valid struct pci_dev, val 2 writable bytes");
    __prism_read_range(dev, 1);
    *val = (unsigned short)__VERIFIER_nondet_int();
    return __VERIFIER_nondet_int();
}

/* include/acpi/acpi_bus.h: bool is_acpi_device_node(const struct fwnode_handle *fwnode) */
bool is_acpi_device_node(const void *fwnode) {
    (void)fwnode;
    ASSUME("linux-w2-is_acpi_device_node: tells whether the firmware node is an ACPI device node (include/acpi/acpi_bus.h:515; the quoted stub at include/linux/acpi.h:844-847 returns false); the node is not dereferenced (it may be NULL) and the result is an arbitrary bool (a superset of the real results)");
    return CHOICE();
}

/* include/linux/netdevice.h: void netif_carrier_off(struct net_device *dev) */
void netif_carrier_off(void *dev) {
    ASSUME("linux-w2-netif_carrier_off: sets the NOCARRIER bit in dev->state, increments carrier_down_count and fires a linkwatch event (net/sched/sch_generic.c:623-631, Linux 6.18); these writes and the link-watch work are not modelled; dev must be a valid struct net_device");
    __prism_read_range(dev, 1);
}

} /* extern "C" */
