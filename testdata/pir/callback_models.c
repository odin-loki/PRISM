// --pir-callback-models (docs/DRIVER_ENV_MODELS.md): driver environment models for callbacks, with the rows of
// callback_models.tsv. Each model-dependent function has a twin with a violation the model does not remove.
// Not a kernel unit: the struct names mimic the kernel's so the rows read like the real ones.
#include <stddef.h>

// ---- id-entry: the probe gets an entry of the driver's own id table (pci, usb, hid) ---------------------------
struct pci_device_id { unsigned long driver_data; };
struct pci_dev { int irq; };
struct board { int irq; };
static const struct board boards[3] = {{10}, {11}, {12}};
static const struct pci_device_id ids[4] = {{0}, {1}, {2}, {0}};  // the last entry is the terminator
struct pci_driver {
    const char* name;
    const struct pci_device_id* id_table;
    int (*probe)(struct pci_dev*, const struct pci_device_id*);
};
// safe for the table's entries: driver_data indexes boards[]
static int good_probe(struct pci_dev* p, const struct pci_device_id* id) {
    p->irq = boards[id->driver_data].irq;
    return 0;
}
// out of bounds for every entry (driver_data + 3 >= 3): a real violation, model or not
static int bad_probe(struct pci_dev* p, const struct pci_device_id* id) {
    p->irq = boards[id->driver_data + 3].irq;
    return 0;
}
struct pci_driver good_driver = {"good", ids, good_probe};
struct pci_driver bad_driver = {"bad", ids, bad_probe};

// ---- range: file_operations read / write counts are at most MAX_RW_COUNT -----------------------------------------
struct file_operations {
    long (*read)(int fd, long count);
    long (*write)(int fd, long count);
};
// divides by zero only for count == MAX_RW_COUNT + 1
static long good_write(int fd, long count) {
    (void)fd;
    return 100 / (count - 2147479553L);
}
// divides by zero for fd == 0, whatever the count
static long bad_read(int fd, long count) {
    (void)count;
    return 100 / fd;
}
struct file_operations fops = {bad_read, good_write};
// the same body, not registered as a callback: no model applies
long unregistered_write(int fd, long count) {
    (void)fd;
    return 100 / (count - 2147479553L);
}

// ---- trailing-alloc: a net_device has the driver's private area after it ------------------------------------------
struct net_device {
    int mtu;
    char pad[60];
    unsigned char priv[];
};
struct my_priv { int a[8]; };
struct net_device* alloc_netdev_mqs(int sizeof_priv, unsigned txqs, unsigned rxqs);
static void* netdev_priv(struct net_device* d) { return d->priv; }
static int good_open(struct net_device* d) {
    struct my_priv* p = netdev_priv(d);
    return p->a[7];
}
// one past the private area (32 bytes): out of bounds even with the model
static int bad_open(struct net_device* d) {
    struct my_priv* p = netdev_priv(d);
    return p->a[8];
}
struct net_device_ops {
    int (*ndo_open)(struct net_device*);
    int (*ndo_stop)(struct net_device*);
};
struct net_device_ops netdev_ops = {good_open, bad_open};
struct net_device* setup_netdev(void) { return alloc_netdev_mqs(sizeof(struct my_priv), 1, 1); }

// ---- buffer: a sysfs show method gets a PAGE_SIZE buffer -----------------------------------------------------------
struct device;
struct device_attribute {
    const char* name;
    long (*show)(struct device*, struct device_attribute*, char*);
};
static long good_show(struct device* dev, struct device_attribute* attr, char* buf) {
    (void)dev;
    (void)attr;
    buf[4000] = 'x';
    return 1;
}
static long bad_show(struct device* dev, struct device_attribute* attr, char* buf) {
    (void)dev;
    (void)attr;
    buf[4096] = 'x';
    return 1;
}
struct device_attribute attrs[2] = {{"good", good_show}, {"bad", bad_show}};
