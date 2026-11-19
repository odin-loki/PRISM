/* Callers of kernel functions this unit declares but does not define. Each
 * call is answered by the library contract (src/prism/pir/models/kernel/):
 * its requires are checked here, its ensures assumed, and the contract is
 * listed as an assumption (PROVED-ASSUMING, never PROVED). Without the
 * contract library these were NEEDS-HARNESS (UNENCODED: call @name). */
#include <stdbool.h>
#include <stddef.h>

struct list_head {
    struct list_head *next, *prev;
};
void INIT_LIST_HEAD(struct list_head *list);
void list_add_tail(struct list_head *node, struct list_head *head);
void list_del(struct list_head *node);
int list_empty(const struct list_head *head);

typedef unsigned int gfp_t;
void *kzalloc(size_t size, gfp_t flags);
void kfree(const void *p);

unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
bool test_bit(unsigned long nr, const volatile unsigned long *addr);
void __set_bit(long nr, volatile unsigned long *addr);
int __ilog2_u32(unsigned int n);

typedef struct {
    struct {
        int counter;
    } refs;
} refcount_t;
void refcount_set(refcount_t *r, int n);
bool refcount_dec_and_test(refcount_t *r);

struct item {
    int key;
    struct list_head link;
};

/* A queue of two items on the stack: add, check, remove. */
int queue_two_ok(int a, int b) {
    struct list_head q;
    struct item x, y;
    x.key = a;
    y.key = b;
    INIT_LIST_HEAD(&q);
    list_add_tail(&x.link, &q);
    list_add_tail(&y.link, &q);
    if (list_empty(&q)) return -1; /* never */
    list_del(&x.link);
    list_del(&y.link);
    return list_empty(&q) ? 0 : 1;
}

/* An allocated object: the NULL check is required (kzalloc may fail). */
int alloc_ok(unsigned n) {
    if (n == 0 || n > 64) return -1;
    int *p = kzalloc(n * sizeof(int), 0);
    if (!p) return -1;
    int v = p[n - 1]; /* zero-filled */
    kfree(p);
    return v;
}

/* Planted: kzalloc's result used without the NULL check. */
int alloc_unchecked_bad(void) {
    int *p = kzalloc(4 * sizeof(int), 0);
    p[0] = 1;
    kfree(p);
    return 0;
}

/* Planted: use after kfree. */
int alloc_uaf_bad(void) {
    int *p = kzalloc(sizeof(int), 0);
    if (!p) return 0;
    kfree(p);
    return *p;
}

/* Every set bit of a two-word bitmap, visited with find_next_bit. */
int count_bits_ok(unsigned long w0, unsigned long w1) {
    unsigned long map[2] = {w0, w1};
    int n = 0;
    for (unsigned long b = find_next_bit(map, 128, 0); b < 128 && n < 2; b = find_next_bit(map, 128, b + 1)) {
        if (!test_bit(b, map)) return -1; /* the contract excludes this */
        n++;
    }
    return n;
}

/* Planted: a bit number one word past the bitmap. */
void set_bit_past_bad(void) {
    unsigned long map[1] = {0};
    __set_bit(64, map);
}

int log2_ok(unsigned v) {
    if (v == 0) return 0;
    int l = __ilog2_u32(v);
    int t[32] = {0};
    return t[l]; /* 0 <= l <= 31 by the contract */
}

int refcount_ok(void) {
    refcount_t r;
    refcount_set(&r, 1);
    return refcount_dec_and_test(&r) ? 0 : 1;
}

/* ---- kernel environment (single-threaded): locks change no memory the
 * caller sees, a static key is any bool, an MMIO read is any value. ---- */
#include <assert.h>
typedef struct spinlock {
    int raw;
} spinlock_t;
struct static_key {
    int enabled;
};
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
bool static_key_false(struct static_key *key);
unsigned int readl(const volatile void *addr);
void writel(unsigned int value, volatile void *addr);
void udelay(unsigned long usecs);

struct dev {
    spinlock_t lock;
    int count;
    unsigned int regs[4];
};

int locked_inc_ok(int start) {
    struct dev d;
    if (start < 0 || start > 100) return 0;
    d.count = start;
    spin_lock(&d.lock);
    d.count++;
    spin_unlock(&d.lock);
    assert(d.count == start + 1);
    return d.count;
}

/* A status register polled a bounded number of times; any value read. */
int poll_ready_ok(void) {
    struct dev d;
    struct static_key trace = {0};
    for (int i = 0; i < 3; i++) {
        unsigned int s = readl(&d.regs[1]);
        if (s & 1u) return i;
        udelay(10);
    }
    if (static_key_false(&trace)) writel(0, &d.regs[0]);
    return -1;
}

/* Planted: the register value is not known, so "bit 0 is set" can fail. */
void readl_known_bad(void) {
    struct dev d;
    assert(readl(&d.regs[0]) & 1u);
}
