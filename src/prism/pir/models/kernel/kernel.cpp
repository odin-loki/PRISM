/* Kernel and data-structure library contracts (docs/CONTRACTS.md,
 * docs/TARGETED_PROVING.md W4). Linked like the libc models (libc_models.cpp
 * link_models) into a unit that declares one of these functions and does not
 * define it: the call is then answered by the contract below instead of
 * stopping at "UNENCODED: call @name" (NEEDS-HARNESS).
 *
 * Each function starts with __prism_contract("kernel:<name> ..."), which
 *   - lists the contract as an assumption of the caller's verdict (a proof
 *     is PROVED-ASSUMING, never PROVED: the real callee is not checked), and
 *   - makes the call refuse a declaration of another signature (translate.cpp
 *     inline_call: argument and return types must match the model's).
 * The requires are checked at the call site (every load, store and range
 * check below goes through the memory model, like user code); the ensures
 * are either the operation itself (loop-free models: list, bit, refcount,
 * arithmetic, allocation) or, for the searching loops (find_*_bit,
 * crc32_le), assumed with __prism_assume on a fresh value.
 *
 * Every contract here is the one proved for the reference implementation in
 * testdata/contracts/<topic>/ (named in the id), and the models themselves
 * are checked against it by testdata/contracts/kmodels/kernel_models.cpp.
 * Single-threaded semantics: the atomic and refcount models are not a
 * concurrency model (listed in the id).
 */
#include "prism_model.h"

extern "C" {


/* ---- linked lists (include/linux/list.h; testdata/contracts/list/list.c) ---- */

struct list_head {
    struct list_head *next, *prev;
};

/* The models reach the fields through `void **` (next at 0, prev at 1):
 * named struct types belong to the unit that links the model, so a model
 * must not depend on the unit naming its struct the same way. */
#define PRISM_NEXT(p) (((void **)(p))[0])
#define PRISM_PREV(p) (((void **)(p))[1])

void INIT_LIST_HEAD(struct list_head *list) {
    __prism_contract("kernel:INIT_LIST_HEAD (testdata/contracts/list/list.c)");
    PRISM_NEXT(list) = list;
    PRISM_PREV(list) = list;
}

void __list_add(struct list_head *node, struct list_head *prev, struct list_head *next) {
    __prism_contract("kernel:__list_add (testdata/contracts/list/list.c)");
    PRISM_PREV(next) = node;
    PRISM_NEXT(node) = next;
    PRISM_PREV(node) = prev;
    PRISM_NEXT(prev) = node;
}

void list_add(struct list_head *node, struct list_head *head) {
    __prism_contract("kernel:list_add (testdata/contracts/list/list.c)");
    void *next = PRISM_NEXT(head);
    PRISM_PREV(next) = node;
    PRISM_NEXT(node) = next;
    PRISM_PREV(node) = head;
    PRISM_NEXT(head) = node;
}

void list_add_tail(struct list_head *node, struct list_head *head) {
    __prism_contract("kernel:list_add_tail (testdata/contracts/list/list.c)");
    void *prev = PRISM_PREV(head);
    PRISM_PREV(head) = node;
    PRISM_NEXT(node) = head;
    PRISM_PREV(node) = prev;
    PRISM_NEXT(prev) = node;
}

void list_del(struct list_head *node) {
    __prism_contract("kernel:list_del (testdata/contracts/list/list.c; NULL in place of LIST_POISON1/2)");
    void *prev = PRISM_PREV(node), *next = PRISM_NEXT(node);
    PRISM_PREV(next) = prev;
    PRISM_NEXT(prev) = next;
    PRISM_NEXT(node) = 0;
    PRISM_PREV(node) = 0;
}

int list_empty(const struct list_head *head) {
    __prism_contract("kernel:list_empty (testdata/contracts/list/list.c)");
    return PRISM_NEXT(head) == (const void *)head;
}

int list_is_head(const struct list_head *list, const struct list_head *head) {
    __prism_contract("kernel:list_is_head (testdata/contracts/list/list.c)");
    return list == head;
}

/* ---- bit operations (include/linux/bitops.h; testdata/contracts/bitmap/bitmap.c) ---- */

#define PRISM_BPL (8UL * sizeof(unsigned long))

/* The kernel's bitmaps are `volatile unsigned long *` for the atomic forms;
 * the models are single-threaded and access the words as plain memory. */
static unsigned long *prism_words(const volatile unsigned long *addr) {
    return const_cast<unsigned long *>(addr);
}

void __set_bit(long nr, volatile unsigned long *addr) {
    __prism_contract("kernel:__set_bit (testdata/contracts/bitmap/bitmap.c)");
    __prism_check(nr >= 0, "MEM-OOB-WRITE", "__set_bit() with a negative bit number");
    prism_words(addr)[(unsigned long)nr / PRISM_BPL] |= 1UL << ((unsigned long)nr % PRISM_BPL);
}

void set_bit(long nr, volatile unsigned long *addr) {
    __prism_contract("kernel:set_bit (testdata/contracts/bitmap/bitmap.c; single-threaded)");
    __prism_check(nr >= 0, "MEM-OOB-WRITE", "set_bit() with a negative bit number");
    prism_words(addr)[(unsigned long)nr / PRISM_BPL] |= 1UL << ((unsigned long)nr % PRISM_BPL);
}

void __clear_bit(long nr, volatile unsigned long *addr) {
    __prism_contract("kernel:__clear_bit (testdata/contracts/bitmap/bitmap.c)");
    __prism_check(nr >= 0, "MEM-OOB-WRITE", "__clear_bit() with a negative bit number");
    prism_words(addr)[(unsigned long)nr / PRISM_BPL] &= ~(1UL << ((unsigned long)nr % PRISM_BPL));
}

void clear_bit(long nr, volatile unsigned long *addr) {
    __prism_contract("kernel:clear_bit (testdata/contracts/bitmap/bitmap.c; single-threaded)");
    __prism_check(nr >= 0, "MEM-OOB-WRITE", "clear_bit() with a negative bit number");
    prism_words(addr)[(unsigned long)nr / PRISM_BPL] &= ~(1UL << ((unsigned long)nr % PRISM_BPL));
}

static bool prism_test_bit(unsigned long nr, const volatile unsigned long *addr) {
    return (prism_words(addr)[nr / PRISM_BPL] >> (nr % PRISM_BPL)) & 1UL;
}

bool test_bit(unsigned long nr, const volatile unsigned long *addr) {
    __prism_contract("kernel:test_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_test_bit(nr, addr);
}

bool _test_bit(unsigned long nr, const volatile unsigned long *addr) {
    __prism_contract("kernel:_test_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_test_bit(nr, addr);
}

bool arch_test_bit(unsigned long nr, const volatile unsigned long *addr) {
    __prism_contract("kernel:arch_test_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_test_bit(nr, addr);
}

bool const_test_bit(unsigned long nr, const volatile unsigned long *addr) {
    __prism_contract("kernel:const_test_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_test_bit(nr, addr);
}

/* find_*_bit: requires the BITS_TO_LONGS(size) words of addr readable;
 * ensures result <= size, and result < size ==> bit `result` is set (clear
 * for the _zero_ forms) and result >= offset. The "every bit before the
 * result is clear" half of the contract (proved for the reference
 * implementation) is not assumed: weaker is sound. */
static void prism_bitmap_readable(const unsigned long *addr, unsigned long size) {
    if (size == 0) return;
    __prism_check(size <= PRISM_MAX_OBJ, "MEM-OOB-READ", "find_*_bit() size beyond any object");
    __prism_read_range(addr, ((size + PRISM_BPL - 1) / PRISM_BPL) * sizeof(unsigned long));
}

static unsigned long prism_find(const unsigned long *addr, unsigned long size, unsigned long offset, bool want) {
    prism_bitmap_readable(addr, size);
    unsigned long r = __VERIFIER_nondet_ulong();
    __prism_assume(r <= size);
    if (r < size) __prism_assume(r >= offset && prism_test_bit(r, addr) == want);
    return r;
}

unsigned long find_first_bit(const unsigned long *addr, unsigned long size) {
    __prism_contract("kernel:find_first_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_find(addr, size, 0, true);
}

unsigned long _find_first_bit(const unsigned long *addr, unsigned long size) {
    __prism_contract("kernel:_find_first_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_find(addr, size, 0, true);
}

unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
    __prism_contract("kernel:find_next_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_find(addr, size, offset, true);
}

unsigned long _find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
    __prism_contract("kernel:_find_next_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_find(addr, size, offset, true);
}

unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size) {
    __prism_contract("kernel:find_first_zero_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_find(addr, size, 0, false);
}

unsigned long _find_first_zero_bit(const unsigned long *addr, unsigned long size) {
    __prism_contract("kernel:_find_first_zero_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_find(addr, size, 0, false);
}

unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
    __prism_contract("kernel:find_next_zero_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_find(addr, size, offset, false);
}

unsigned long _find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
    __prism_contract("kernel:_find_next_zero_bit (testdata/contracts/bitmap/bitmap.c)");
    return prism_find(addr, size, offset, false);
}

/* ---- integer helpers (include/linux/log2.h, math64.h; testdata/contracts/arith/arith.c) ---- */

int __ilog2_u32(unsigned int n) {
    __prism_contract("kernel:__ilog2_u32 (testdata/contracts/arith/arith.c)");
    return n ? 31 - __builtin_clz(n) : -1;
}

int __ilog2_u64(unsigned long long n) {
    __prism_contract("kernel:__ilog2_u64 (testdata/contracts/arith/arith.c)");
    return n ? 63 - __builtin_clzll(n) : -1;
}

unsigned long __roundup_pow_of_two(unsigned long n) {
    __prism_contract("kernel:__roundup_pow_of_two (testdata/contracts/arith/arith.c)");
    __prism_check(n != 0 && n <= (1UL << (PRISM_BPL - 1)), "INT-SHIFT-OVERFLOW",
                  "__roundup_pow_of_two() of 0 or of a value above the largest power of two (1UL << BITS_PER_LONG)");
    if (n == 1) return 1;
    return 1UL << (PRISM_BPL - (unsigned long)__builtin_clzl(n - 1));
}

unsigned long long div_u64(unsigned long long dividend, unsigned int divisor) {
    __prism_contract("kernel:div_u64 (testdata/contracts/arith/arith.c)");
    __prism_check(divisor != 0, "INT-DIV-ZERO", "div_u64() by zero");
    return dividend / divisor;
}

unsigned long long div_u64_rem(unsigned long long dividend, unsigned int divisor, unsigned int *remainder) {
    __prism_contract("kernel:div_u64_rem (testdata/contracts/arith/arith.c)");
    __prism_check(divisor != 0, "INT-DIV-ZERO", "div_u64_rem() by zero");
    *remainder = (unsigned int)(dividend % divisor);
    return dividend / divisor;
}

/* ---- reference counts (include/linux/refcount.h, kref.h; testdata/contracts/refcount/refcount.c) ---- */

typedef struct {
    int counter;
} atomic_t;

typedef struct {
    atomic_t refs;
} refcount_t;

struct kref {
    refcount_t refcount;
};

#define PRISM_REFCOUNT_SATURATED ((int)0xC0000000u) /* INT_MIN / 2 */
/* the counter is the first (only) int of atomic_t, refcount_t and kref:
 * reached as an int, not through the unit's struct type names */
#define PRISM_CTR(p) (*(int *)(p))

int atomic_read(const atomic_t *v) {
    __prism_contract("kernel:atomic_read (single-threaded)");
    return PRISM_CTR(v);
}

void atomic_set(atomic_t *v, int i) {
    __prism_contract("kernel:atomic_set (single-threaded)");
    PRISM_CTR(v) = i;
}

void atomic_inc(atomic_t *v) {
    __prism_contract("kernel:atomic_inc (single-threaded; wraps as the kernel's atomic_t does)");
    PRISM_CTR(v) = (int)((unsigned)PRISM_CTR(v) + 1u);
}

void atomic_dec(atomic_t *v) {
    __prism_contract("kernel:atomic_dec (single-threaded; wraps)");
    PRISM_CTR(v) = (int)((unsigned)PRISM_CTR(v) - 1u);
}

bool atomic_dec_and_test(atomic_t *v) {
    __prism_contract("kernel:atomic_dec_and_test (single-threaded; wraps)");
    PRISM_CTR(v) = (int)((unsigned)PRISM_CTR(v) - 1u);
    return PRISM_CTR(v) == 0;
}

void refcount_set(refcount_t *r, int n) {
    __prism_contract("kernel:refcount_set (testdata/contracts/refcount/refcount.c; single-threaded)");
    PRISM_CTR(r) = n;
}

unsigned int refcount_read(const refcount_t *r) {
    __prism_contract("kernel:refcount_read (testdata/contracts/refcount/refcount.c; single-threaded)");
    return (unsigned int)PRISM_CTR(r);
}

/* An increment from 0 (use after free) or of a negative (saturated) or
 * maximal counter saturates: the object is leaked, never freed early. */
static void prism_ref_inc(int *c) {
    int old = *c;
    if (old <= 0 || old == 0x7fffffff) *c = PRISM_REFCOUNT_SATURATED;
    else *c = old + 1;
}

void refcount_inc(refcount_t *r) {
    __prism_contract("kernel:refcount_inc (testdata/contracts/refcount/refcount.c; saturating, single-threaded)");
    prism_ref_inc(&PRISM_CTR(r));
}

bool refcount_dec_and_test(refcount_t *r) {
    __prism_contract("kernel:refcount_dec_and_test (testdata/contracts/refcount/refcount.c; saturating, single-threaded)");
    int old = PRISM_CTR(r);
    if (old < 0) {
        PRISM_CTR(r) = PRISM_REFCOUNT_SATURATED;
        return false;
    }
    if (old == 0) return false;
    PRISM_CTR(r) = old - 1;
    return old == 1;
}

void kref_init(struct kref *k) {
    __prism_contract("kernel:kref_init (testdata/contracts/refcount/refcount.c)");
    PRISM_CTR(k) = 1;
}

void kref_get(struct kref *k) {
    __prism_contract("kernel:kref_get (testdata/contracts/refcount/refcount.c; saturating, single-threaded)");
    prism_ref_inc(&PRISM_CTR(k));
}

/* ---- allocation (include/linux/slab.h; testdata/contracts/alloc/pool.c for the
 * free-list shape). May fail (NULL), like malloc; kzalloc / kcalloc
 * zero-fill; kfree(NULL) is a no-op. ---- */

typedef unsigned int gfp_t;

/* __prism_alloc takes its kind and initial contents as constants */
static void *prism_kalloc(size_t n, bool zero) {
    if (n >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) {
        __prism_alloc_failed();
        return 0;
    }
    return zero ? __prism_alloc(n, PRISM_HEAP, PRISM_ZERO) : __prism_alloc(n, PRISM_HEAP, PRISM_UNINIT);
}

static void *prism_kalloc_array(size_t n, size_t size, bool zero) {
    size_t t;
    if (__builtin_mul_overflow(n, size, &t)) return 0; /* the kernel returns NULL on overflow */
    return prism_kalloc(t, zero);
}

void *kmalloc(size_t size, gfp_t flags) {
    __prism_contract("kernel:kmalloc (may fail; contents uninitialised)");
    (void)flags;
    return prism_kalloc(size, false);
}

void *kmalloc_noprof(size_t size, gfp_t flags) {
    __prism_contract("kernel:kmalloc_noprof (may fail; contents uninitialised)");
    (void)flags;
    return prism_kalloc(size, false);
}

void *kzalloc(size_t size, gfp_t flags) {
    __prism_contract("kernel:kzalloc (may fail; zero-filled)");
    (void)flags;
    return prism_kalloc(size, true);
}

void *kzalloc_noprof(size_t size, gfp_t flags) {
    __prism_contract("kernel:kzalloc_noprof (may fail; zero-filled)");
    (void)flags;
    return prism_kalloc(size, true);
}

void *kmalloc_array(size_t n, size_t size, gfp_t flags) {
    __prism_contract("kernel:kmalloc_array (may fail; NULL on n * size overflow)");
    (void)flags;
    return prism_kalloc_array(n, size, false);
}

void *kmalloc_array_noprof(size_t n, size_t size, gfp_t flags) {
    __prism_contract("kernel:kmalloc_array_noprof (may fail; NULL on n * size overflow)");
    (void)flags;
    return prism_kalloc_array(n, size, false);
}

void *kcalloc(size_t n, size_t size, gfp_t flags) {
    __prism_contract("kernel:kcalloc (may fail; zero-filled; NULL on n * size overflow)");
    (void)flags;
    return prism_kalloc_array(n, size, true);
}

void kfree(const void *p) {
    __prism_contract("kernel:kfree (NULL is a no-op)");
    __prism_free((void *)p, PRISM_HEAP);
}

/* ---- strings and checksums (lib/string.c, lib/crc32.c; testdata/contracts/string, crc) ---- */

/* strscpy: copies at most count - 1 bytes and a NUL; the length copied, or
 * -E2BIG (-7) when src did not fit (dest then holds a terminated prefix). */
ssize_t strscpy(char *dest, const char *src, size_t count) {
    __prism_contract("kernel:strscpy (testdata/contracts/string/strscpy.c)");
    if (count == 0) return -7;
    __prism_check(count <= PRISM_MAX_OBJ, "MEM-OOB-WRITE", "strscpy() count beyond any object");
    size_t i = 0;
    while (i < count) {
        char c = src[i];
        dest[i] = c;
        if (!c) return (ssize_t)i;
        i++;
    }
    dest[count - 1] = 0;
    return -7;
}

/* crc32_le: reads the len bytes of p; the value is not modelled (any u32). */
unsigned int crc32_le(unsigned int crc, const unsigned char *p, size_t len) {
    __prism_contract("kernel:crc32_le (reads p[0..len-1]; value unspecified)");
    (void)crc;
    if (len) __prism_read_range(p, len);
    return (unsigned int)__VERIFIER_nondet_int();
}

/* ---- kernel environment: the callees OS-Proving's core tier was blocked on
 * most often (docs/CONTRACTS.md, priority table). Each is the weakest
 * contract that is still true: a configuration or static-key test is any
 * bool, MMIO reads are any value, delays and locks change no memory the
 * caller can see (single-threaded: locks are not a concurrency model, and
 * that is in each id). ---- */

struct static_key;
typedef struct spinlock spinlock_t;
typedef struct raw_spinlock raw_spinlock_t;
struct mutex;

bool mem_alloc_profiling_enabled(void) {
    __prism_contract("kernel:mem_alloc_profiling_enabled (configuration: any value)");
    return __VERIFIER_nondet_int() != 0;
}

bool static_key_false(struct static_key *key) {
    __prism_contract("kernel:static_key_false (static key: any value)");
    (void)key;
    return __VERIFIER_nondet_int() != 0;
}

bool static_key_true(struct static_key *key) {
    __prism_contract("kernel:static_key_true (static key: any value)");
    (void)key;
    return __VERIFIER_nondet_int() != 0;
}

bool arch_static_branch(struct static_key *const key, const bool branch) {
    __prism_contract("kernel:arch_static_branch (static key: any value)");
    (void)key;
    (void)branch;
    return __VERIFIER_nondet_int() != 0;
}

bool arch_static_branch_jump(struct static_key *const key, const bool branch) {
    __prism_contract("kernel:arch_static_branch_jump (static key: any value)");
    (void)key;
    (void)branch;
    return __VERIFIER_nondet_int() != 0;
}

/* spinlock_t's first member is the raw lock (include/linux/spinlock.h) */
raw_spinlock_t *spinlock_check(spinlock_t *lock) {
    __prism_contract("kernel:spinlock_check (returns &lock->rlock, offset 0)");
    return (raw_spinlock_t *)lock;
}

void spin_lock(spinlock_t *lock) {
    __prism_contract("kernel:spin_lock (single-threaded: no memory effect)");
    (void)lock;
}

void spin_unlock(spinlock_t *lock) {
    __prism_contract("kernel:spin_unlock (single-threaded: no memory effect)");
    (void)lock;
}

void spin_lock_bh(spinlock_t *lock) {
    __prism_contract("kernel:spin_lock_bh (single-threaded: no memory effect)");
    (void)lock;
}

void spin_unlock_bh(spinlock_t *lock) {
    __prism_contract("kernel:spin_unlock_bh (single-threaded: no memory effect)");
    (void)lock;
}

void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags) {
    __prism_contract("kernel:spin_unlock_irqrestore (single-threaded: no memory effect)");
    (void)lock;
    (void)flags;
}

void mutex_lock(struct mutex *lock) {
    __prism_contract("kernel:mutex_lock (single-threaded: no memory effect)");
    (void)lock;
}

void mutex_unlock(struct mutex *lock) {
    __prism_contract("kernel:mutex_unlock (single-threaded: no memory effect)");
    (void)lock;
}

void rcu_read_lock(void) { __prism_contract("kernel:rcu_read_lock (single-threaded: no memory effect)"); }

void rcu_read_unlock(void) { __prism_contract("kernel:rcu_read_unlock (single-threaded: no memory effect)"); }

void cpu_relax(void) { __prism_contract("kernel:cpu_relax (no memory effect)"); }

void might_resched(void) { __prism_contract("kernel:might_resched (no memory effect)"); }

void udelay(unsigned long usecs) {
    __prism_contract("kernel:udelay (no memory effect)");
    (void)usecs;
}

void usleep_range(unsigned long min, unsigned long max) {
    __prism_contract("kernel:usleep_range (no memory effect)");
    (void)min;
    (void)max;
}

void msleep(unsigned int msecs) {
    __prism_contract("kernel:msleep (no memory effect)");
    (void)msecs;
}

unsigned long msecs_to_jiffies(const unsigned int m) {
    __prism_contract("kernel:msecs_to_jiffies (HZ is configuration: any value)");
    (void)m;
    return __VERIFIER_nondet_ulong();
}

/* MMIO: the address is device memory, not an object of the program; a read
 * is any value, a write changes no program memory. */
unsigned int readl(const volatile void *addr) {
    __prism_contract("kernel:readl (MMIO: any value)");
    (void)addr;
    return (unsigned int)__VERIFIER_nondet_int();
}

unsigned int __readl(const volatile void *addr) {
    __prism_contract("kernel:__readl (MMIO: any value)");
    (void)addr;
    return (unsigned int)__VERIFIER_nondet_int();
}

void writel(unsigned int value, volatile void *addr) {
    __prism_contract("kernel:writel (MMIO: no program memory written)");
    (void)value;
    (void)addr;
}

}  // extern "C"
