/* PIR tasks: Linux kernel API specs, W1 groups (docs/KERNEL_MODELS.md). Run with
 * `--kernel-api linux`: every function here that uses a spec is PROVED-ASSUMING
 * (the specs' assumptions are listed), a defect in the caller is FAILED; without
 * --kernel-api each call is `UNENCODED: call @f` (NEEDS-HARNESS). The declarations
 * below are the kernel's own prototypes; opaque kernel objects are stood for by
 * local char objects. */

void i2c_set_clientdata(void *client, void *data);
void pci_set_drvdata(void *pdev, void *data);
void lock_sock(void *sk);
void release_sock(void *sk);
unsigned char nla_get_u8(const void *nla);
_Bool ceph_has_room(void **p, void *end, unsigned long n);
_Bool is_valid_ether_addr(const unsigned char *addr);
void finish_wait(void *wq_head, void *wq_entry);
long prepare_to_wait_event(void *wq_head, void *wq_entry, int state);
struct completion { unsigned int done; };
unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout);

/* ---- completion ---- */
int wait_for_completion_timeout_ok(void) {
    struct completion c = {0};
    unsigned long r = wait_for_completion_timeout(&c, 100);
    (void)r; /* timed out (0) or remaining time: both are normal returns */
    return 0;
}
int wait_for_completion_timeout_bad(void) { /* the timeout result is not checked: r may be 0 */
    struct completion c = {0};
    unsigned long r = wait_for_completion_timeout(&c, 100);
    return (int)(100 / r);
}

/* ---- wait queues ---- */
int finish_wait_ok(void) {
    char wq, entry; /* stand for struct wait_queue_head and struct wait_queue_entry */
    finish_wait(&wq, &entry);
    return 0;
}
int prepare_to_wait_event_ok(void) {
    char wq, entry;
    long r = prepare_to_wait_event(&wq, &entry, 2 /* TASK_UNINTERRUPTIBLE: never -ERESTARTSYS */);
    return r == 0 ? 0 : -1;
}

/* ---- driver data ---- */
int i2c_set_clientdata_ok(void) {
    char cli, priv; /* stands for a struct i2c_client and the driver data */
    i2c_set_clientdata(&cli, &priv);
    return 0;
}
int pci_set_drvdata_ok(void) {
    char pdev, priv; /* stands for a struct pci_dev and the driver data */
    pci_set_drvdata(&pdev, &priv);
    return 0;
}

/* ---- socket lock ---- */
int lock_sock_ok(void) {
    char sk; /* stands for a struct sock */
    lock_sock(&sk);
    release_sock(&sk);
    return 0;
}

/* ---- netlink attributes ---- */
int nla_get_u8_ok(void) {
    /* a u8 attribute: nla_len = 5 (4-byte header + 1 payload byte), nla_type = 1 */
    unsigned char attr[8] = { 5, 0, 1, 0, 0x2a, 0, 0, 0 };
    return nla_get_u8(attr) == 0x2a ? 0 : -1;
}
int nla_get_u8_bad(void) { /* header only, no payload byte: read past the object */
    unsigned char attr[4] = { 4, 0, 1, 0 };
    return nla_get_u8(attr);
}

/* ---- ceph decoding ---- */
int ceph_has_room_ok(void) {
    unsigned char buf[16];
    void *cur = buf + 4;
    return ceph_has_room(&cur, buf + 16, 12) ? 0 : -1;
}
int ceph_has_room_bad(void) { /* p is not a valid pointer: *p is read */
    return ceph_has_room((void **)0, (void *)0, 4);
}

/* ---- ethernet addresses ---- */
int is_valid_ether_addr_ok(void) {
    unsigned char addr[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    return is_valid_ether_addr(addr) ? 0 : -1;
}
int is_valid_ether_addr_bad(void) { /* unicast: six bytes are read, a 3-byte object is an out-of-bounds read */
    unsigned char addr[3] = { 0x00, 0x11, 0x22 };
    return is_valid_ether_addr(addr) ? 0 : -1;
}
