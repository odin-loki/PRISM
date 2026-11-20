/* PIR tasks: Linux kernel API specs (docs/KERNEL_MODELS.md). Run with
 * `--kernel-api linux`: every function here that uses a spec is PROVED-ASSUMING
 * (the specs' assumptions are listed), a defect in the caller is FAILED; without
 * --kernel-api each call is `UNENCODED: call @f` (NEEDS-HARNESS). The kernel
 * headers are not needed: the declarations below are the kernel's own. */

void *__kmalloc_noprof(unsigned long size, unsigned int flags);
void kfree(const void *p);
void *krealloc_node_align_noprof(const void *p, unsigned long n, unsigned long align, unsigned int flags, int nid);
char *kstrdup(const char *s, unsigned int gfp);
void *__vmalloc_noprof(unsigned long size, unsigned int gfp);
void vfree(const void *p);

struct mutex { long owner; long wait[4]; };
void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);
int mutex_trylock(struct mutex *lock);
typedef struct { int raw; } raw_spinlock_t;
unsigned long _raw_spin_lock_irqsave(raw_spinlock_t *lock);
void _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags);

unsigned long _copy_from_user(void *to, const void *from, unsigned long n);
unsigned long _copy_to_user(void *to, const void *from, unsigned long n);
char user_page[64]; /* stands for a user buffer: user addresses are not modelled */

unsigned int ioread32(const void *addr);
void iowrite32(unsigned int v, void *addr);
void *ioremap(unsigned long offset, unsigned long size);

int _printk(const char *fmt, ...);
void _dev_err(const void *dev, const char *fmt, ...);
void msleep(unsigned int msecs);
void panic(const char *fmt, ...);

#define GFP_KERNEL 0xcc0u
#define __GFP_ZERO 0x100u

/* ---- allocation ---- */
int alloc_ok(void) {
    char *p = __kmalloc_noprof(8, GFP_KERNEL);
    if (!p) return -12;
    p[7] = 1;
    kfree(p);
    return 0;
}
int alloc_nocheck_bad(void) { /* kmalloc can return NULL */
    char *p = __kmalloc_noprof(8, GFP_KERNEL);
    p[0] = 1;
    kfree(p);
    return 0;
}
int zalloc_ok(void) { /* __GFP_ZERO: the bytes are initialised and zero */
    unsigned char *p = __kmalloc_noprof(8, GFP_KERNEL | __GFP_ZERO);
    if (!p) return -12;
    int r = 100 / (p[3] + 1);
    kfree(p);
    return r;
}
int malloc_uninit_bad(void) { /* without __GFP_ZERO the bytes are uninitialised */
    unsigned char *p = __kmalloc_noprof(8, GFP_KERNEL);
    if (!p) return -12;
    int r = p[3];
    kfree(p);
    return r;
}
int double_free_bad(void) {
    char *p = __kmalloc_noprof(4, GFP_KERNEL);
    if (!p) return -12;
    kfree(p);
    kfree(p);
    return 0;
}
int use_after_free_bad(void) {
    char *p = __kmalloc_noprof(4, GFP_KERNEL | __GFP_ZERO);
    if (!p) return -12;
    kfree(p);
    return p[0];
}
int oob_bad(void) {
    char *p = __kmalloc_noprof(4, GFP_KERNEL);
    if (!p) return -12;
    p[4] = 0;
    kfree(p);
    return 0;
}
int kfree_null_ok(void) {
    kfree((void *)0);
    return 0;
}
int krealloc_ok(void) { /* the bytes up to the old size are kept */
    unsigned char *p = __kmalloc_noprof(4, GFP_KERNEL | __GFP_ZERO);
    if (!p) return -12;
    p[1] = 9;
    unsigned char *q = krealloc_node_align_noprof(p, 16, 1, GFP_KERNEL, -1);
    if (!q) {
        kfree(p);
        return -12;
    }
    int r = q[1];
    kfree(q);
    return r;
}
int kstrdup_ok(void) {
    char *s = kstrdup("abc", GFP_KERNEL);
    if (!s) return -12;
    int r = s[2];
    kfree(s);
    return r;
}
int vmalloc_ok(void) {
    char *p = __vmalloc_noprof(4096, GFP_KERNEL | __GFP_ZERO);
    if (!p) return -12;
    p[4095] = 1;
    vfree(p);
    return 0;
}

/* ---- locking ---- */
int lock_ok(void) {
    struct mutex m = {0};
    mutex_lock_nested(&m, 0);
    mutex_unlock(&m);
    return 0;
}
int lock_null_bad(void) {
    mutex_lock_nested((struct mutex *)0, 0);
    return 0;
}
int trylock_ok(void) {
    struct mutex m = {0};
    if (!mutex_trylock(&m)) return -16;
    mutex_unlock(&m);
    return 0;
}
int spin_irqsave_ok(void) {
    raw_spinlock_t l = {0};
    unsigned long flags = _raw_spin_lock_irqsave(&l);
    _raw_spin_unlock_irqrestore(&l, flags);
    return 0;
}

/* ---- user access ---- */
int copy_from_user_ok(void) {
    char buf[8];
    if (_copy_from_user(buf, user_page, 8)) return -14;
    return buf[7] & 1; /* the bytes are initialised (arbitrary) */
}
int copy_from_user_trust_bad(void) { /* user bytes are arbitrary: buf[0] can be 0 */
    char buf[8];
    if (_copy_from_user(buf, user_page, 8)) return -14;
    return 100 / buf[0];
}
int copy_from_user_oob_bad(void) {
    char buf[8];
    if (_copy_from_user(buf, user_page, 16)) return -14; /* longer than buf */
    return 0;
}
int copy_to_user_ok(void) {
    char buf[8] = {0};
    return _copy_to_user(user_page, buf, 8) ? -14 : 0;
}
int copy_to_user_oob_bad(void) {
    char buf[8] = {0};
    return _copy_to_user(user_page, buf, 16) ? -14 : 0; /* reads past buf */
}

/* ---- MMIO ---- */
int mmio_ok(void) {
    char *regs = ioremap(0xfee00000ul, 4096);
    if (!regs) return -5;
    iowrite32(1, regs + 4);
    unsigned int v = ioread32(regs);
    return 100 / (int)((v & 0xff) | 1); /* any register value */
}
int mmio_trust_bad(void) { /* a register read can be 0 */
    char *regs = ioremap(0xfee00000ul, 4096);
    if (!regs) return -5;
    unsigned int v = ioread32(regs);
    return 100 / (int)(v & 0xff);
}

/* ---- delays, logging, panic ---- */
int log_delay_ok(void) {
    int x = 3;
    _dev_err((const void *)0, "x=%d", x);
    _printk("hello %s %d\n", "world", x);
    msleep(10);
    return 100 / x; /* logging and delays leave x alone */
}
int log_delay_bad(void) {
    int x = 3;
    _dev_err((const void *)0, "x=%d", x);
    msleep(10);
    return 100 / (x - 3);
}
int panic_bad(int n) {
    if (n == 5) panic("bad %d", n);
    return n;
}
int panic_ok(int n) {
    if ((n & 3) == 5) panic("unreachable %d", n);
    return n;
}

/* ---- container_of / offsetof: plain pointer arithmetic, no spec ---- */
struct list_head { struct list_head *next, *prev; };
struct item { int tag; struct list_head node; int val; };
#define offsetof_(t, m) __builtin_offsetof(t, m)
#define container_of_(ptr, type, member) ((type *)((char *)(ptr) - offsetof_(type, member)))
int container_of_ok(void) {
    struct item it = {7, {0, 0}, 11};
    struct list_head *n = &it.node;
    struct item *back = container_of_(n, struct item, node);
    return 100 / (back->val - 10); /* back == &it */
}
int container_of_bad(void) {
    struct item it = {7, {0, 0}, 11};
    struct list_head *n = &it.node;
    struct item *back = container_of_(n, struct item, node);
    return 100 / (back->val - 11);
}
