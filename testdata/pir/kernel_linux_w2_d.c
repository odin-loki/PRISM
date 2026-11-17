/* PIR tasks: Linux kernel API specs, W2 group d (docs/KERNEL_MODELS.md). Run with
 * `--kernel-api linux`. Opaque kernel objects are stood for by local char objects. */

void rtnl_lock(void);
int reset_control_assert(void *rstc);
unsigned int jiffies_to_msecs(const unsigned long j);
int pci_read_config_word(const void *dev, int where, unsigned short *val);
_Bool is_acpi_device_node(const void *fwnode);
void netif_carrier_off(void *dev);

int rtnl_lock_ok(void) {
    rtnl_lock();
    return 0;
}
int rtnl_lock_bad(void) { /* a defect after the call */
    int z = 0;
    rtnl_lock();
    return 100 / z;
}

int reset_control_assert_ok(void) {
    char rstc; /* stands for a struct reset_control */
    int r = reset_control_assert(&rstc);
    (void)r; /* 0 or an errno: both are normal returns */
    return 0;
}
int reset_control_assert_bad(void) { /* r == -1 is a possible failure: division by zero */
    char rstc;
    int r = reset_control_assert(&rstc);
    return 100 / (r + 1);
}

int jiffies_to_msecs_ok(void) {
    unsigned int ms = jiffies_to_msecs(10UL);
    (void)ms;
    return 0;
}
int jiffies_to_msecs_bad(void) { /* the result may be 0 */
    unsigned int ms = jiffies_to_msecs(10UL);
    return (int)(100 / ms);
}

int pci_read_config_word_ok(void) {
    char dev; /* stands for a struct pci_dev */
    unsigned short v = 0;
    int r = pci_read_config_word(&dev, 0, &v);
    (void)r;
    return (int)(v & 1); /* any value is acceptable */
}
int pci_read_config_word_bad(void) { /* val is a 1-byte object: 2-byte out-of-bounds write */
    char dev;
    unsigned char v[1] = { 0 };
    pci_read_config_word(&dev, 0, (unsigned short *)v);
    return v[0];
}

int is_acpi_device_node_ok(void) {
    char fwnode; /* stands for a struct fwnode_handle */
    _Bool b = is_acpi_device_node(&fwnode);
    (void)b;
    return 0;
}
int is_acpi_device_node_bad(void) { /* the result may be false: division by zero */
    char fwnode;
    _Bool b = is_acpi_device_node(&fwnode);
    return 100 / (int)b;
}

int netif_carrier_off_ok(void) {
    char dev; /* stands for a struct net_device */
    netif_carrier_off(&dev);
    return 0;
}
int netif_carrier_off_bad(void) { /* dev is NULL: dev->state is accessed */
    netif_carrier_off((void *)0);
    return 0;
}
