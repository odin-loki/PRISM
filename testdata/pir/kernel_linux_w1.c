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

/* ---- W1 more: completion, wake-up, netlink attributes, Ethernet addresses, DMA error ---- */
void complete(struct completion *x);
void wait_for_completion(struct completion *x);
int __wake_up(void *wq_head, unsigned int mode, int nr, void *key);
unsigned short nla_len(const void *nla);
void *nla_data(const void *nla);
unsigned int nla_get_u32(const void *nla);
int nla_total_size(int payload);
_Bool ether_addr_equal(const unsigned char *a, const unsigned char *b);
void ether_addr_copy(unsigned char *dst, const unsigned char *src);
int dma_mapping_error(void *dev, unsigned long long dma_addr);

int complete_ok(void) {
    struct completion c = {0};
    complete(&c);
    return 0;
}
int complete_bad(void) { /* x is NULL: the done counter is read */
    complete((struct completion *)0);
    return 0;
}
int wait_for_completion_ok(void) {
    struct completion c = {1};
    wait_for_completion(&c);
    return 0;
}
int wait_for_completion_bad(void) {
    wait_for_completion((struct completion *)0);
    return 0;
}
int __wake_up_ok(void) {
    char wq; /* stands for a struct wait_queue_head */
    int n = __wake_up(&wq, 3 /* TASK_NORMAL */, 1, (void *)0);
    (void)n; /* any result is a normal return */
    return 0;
}
int __wake_up_bad(void) { /* the queue head is NULL */
    return __wake_up((void *)0, 3, 1, (void *)0);
}
int nla_len_ok(void) {
    unsigned short attr[4] = { 8, 1, 0, 0 }; /* nla_len = 8: payload 4 (u16-aligned, as a real attribute) */
    return nla_len(attr) == 4 ? 0 : -1;
}
int nla_len_bad(void) { /* header of 4 bytes with nla_len 4: payload length 0 */
    unsigned short attr[2] = { 4, 1 };
    return 100 / nla_len(attr);
}
int nla_data_ok(void) {
    unsigned char attr[8] = { 5, 0, 1, 0, 0x2a, 0, 0, 0 };
    return *(unsigned char *)nla_data(attr) == 0x2a ? 0 : -1;
}
int nla_data_bad(void) { /* header only: the payload pointer is one past the object */
    unsigned char attr[4] = { 4, 0, 1, 0 };
    return *(unsigned char *)nla_data(attr);
}
int nla_get_u32_ok(void) {
    unsigned int attr[2] = { 8, 0x2a }; /* header word, then the payload word */
    return nla_get_u32(attr) == 0x2a ? 0 : -1;
}
int nla_get_u32_bad(void) { /* header only, no payload word */
    unsigned int attr[1] = { 4 };
    return (int)nla_get_u32(attr);
}
int nla_total_size_ok(void) {
    return (nla_total_size(1) == 8 && nla_total_size(4) == 8 && nla_total_size(5) == 12) ? 0 : -1;
}
int nla_total_size_bad(void) { /* nla_total_size(0) is 4: a division by zero */
    return 100 / (nla_total_size(0) - 4);
}
int ether_addr_equal_ok(void) {
    unsigned char a[6] = { 0, 1, 2, 3, 4, 5 }, b[6] = { 0, 1, 2, 3, 4, 5 };
    if (!ether_addr_equal(a, b)) return -1;
    b[5] = 9;
    return ether_addr_equal(a, b) ? -1 : 0;
}
int ether_addr_equal_bad(void) { /* a 3-byte object: out-of-bounds read */
    unsigned char a[3] = { 0, 1, 2 }, b[6] = { 0, 1, 2, 0, 0, 0 };
    return ether_addr_equal(a, b) ? 0 : -1;
}
int ether_addr_copy_ok(void) {
    unsigned char s[6] = { 1, 2, 3, 4, 5, 6 }, d[6] = { 0 };
    ether_addr_copy(d, s);
    return d[0] == 1 && d[5] == 6 ? 0 : -1;
}
int ether_addr_copy_bad(void) { /* destination of 4 bytes: out-of-bounds write */
    unsigned char s[6] = { 1, 2, 3, 4, 5, 6 }, d[4] = { 0 };
    ether_addr_copy(d, s);
    return d[0];
}
int dma_mapping_error_ok(void) {
    char dev;
    if (dma_mapping_error(&dev, 0x1000ULL) != 0) return -1;
    return dma_mapping_error(&dev, ~0ULL) == -12 ? 0 : -1;
}
int dma_mapping_error_bad(void) { /* a valid address gives 0: a division by zero */
    char dev;
    return 100 / dma_mapping_error(&dev, 0x1000ULL);
}
