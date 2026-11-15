/* The kernel library contracts (src/prism/pir/models/kernel/kernel.cpp)
 * checked by PRISM itself, the way the libc models are
 * (testdata/conformance/libc-models): this unit #includes the model source,
 * so the pir stage analyses the model code (inlined at each harness call,
 * nothing assumed about it), and states the contract of
 * testdata/contracts/<topic>/ with assert(). Objects are built here with
 * symbolic contents; the harness parameters are scalars (Law 6). */
#include <assert.h>
#include "../../../src/prism/pir/models/libc/prism_model.h"
#include "../../../src/prism/pir/models/kernel/kernel.cpp"

extern "C" void __VERIFIER_assume(int);

/* ---- lists: k = 0..3 nodes after INIT_LIST_HEAD + list_add_tail ---- */

static bool klist_ok(const list_head *n, int k) {
    const list_head *h = &n[0], *p = h;
    for (int i = 0; i <= k; i++) {
        if (p->next->prev != p) return false;
        p = p->next;
        if ((p == h) != (i == k)) return false;
    }
    return true;
}

/* Split in three (one 300 s budget each): the whole sequence in one harness
 * spent the function budget (TIMEOUT). */
static void kbuild(list_head *n, int k) {
    INIT_LIST_HEAD(&n[0]);
    for (int i = 1; i <= k; i++) list_add_tail(&n[i], &n[0]);
}

extern "C" void kmodel_list_add_tail_contract(int k) {
    list_head n[4];
    if (k < 0 || k > 2) return;
    INIT_LIST_HEAD(&n[0]);
    assert(list_empty(&n[0]));
    for (int i = 1; i <= k; i++) list_add_tail(&n[i], &n[0]);
    assert(klist_ok(n, k) && list_empty(&n[0]) == (k == 0));
}

extern "C" void kmodel_list_add_contract(int k) {
    list_head n[4], e;
    if (k < 0 || k > 2) return;
    kbuild(n, k);
    list_head *first = n[0].next;
    list_add(&e, &n[0]);
    assert(n[0].next == &e && e.next == first && e.prev == &n[0] && klist_ok(n, k + 1));
    assert(first->prev == &e);
    list_del(&e);
    assert(e.next == 0 && e.prev == 0 && n[0].next == first && klist_ok(n, k));
}

extern "C" void kmodel_list_del_contract(int k, int j) {
    list_head n[4];
    if (k < 1 || k > 2 || j < 1 || j > k) return;
    kbuild(n, k);
    list_del(&n[j]);
    assert(n[j].next == 0 && n[j].prev == 0);
    assert(list_is_head(n[0].prev->next, &n[0]));
    assert(list_empty(&n[0]) == (k == 1));
}

extern "C" void kmodel___list_add_contract(void) {
    list_head a, b, c;
    a.next = &b;
    b.prev = &a;
    __list_add(&c, &a, &b);
    assert(a.next == &c && c.prev == &a && c.next == &b && b.prev == &c);
}

/* Planted twin: list_del of a node whose neighbours are NULL. */
extern "C" void kmodel_list_del_null_bad(void) {
    list_head x;
    x.next = 0;
    x.prev = 0;
    list_del(&x);
}

/* ---- bits ---- */

extern "C" void kmodel_bits_contract(unsigned long w0, unsigned long w1, long nr, unsigned long j) {
    unsigned long a[2] = {w0, w1};
    if (nr < 0 || nr >= 128 || j >= 128) return;
    bool before = test_bit(j, a);
    __set_bit(nr, a);
    assert(test_bit(nr, a) && _test_bit(nr, a) && arch_test_bit(nr, a) && const_test_bit(nr, a));
    if ((unsigned long)nr != j) assert(test_bit(j, a) == before);
    clear_bit(nr, a);
    assert(!test_bit(nr, a));
    set_bit(nr, a);
    __clear_bit(nr, a);
    assert(!test_bit(nr, a));
    if ((unsigned long)nr != j) assert(test_bit(j, a) == before);
}

extern "C" void kmodel_find_contract(unsigned long w0, unsigned long w1, unsigned long size, unsigned long off) {
    unsigned long a[2] = {w0, w1};
    if (size > 128) return;
    unsigned long r = find_first_bit(a, size);
    assert(r <= size);
    if (r < size) assert(test_bit(r, a));
    r = find_next_bit(a, size, off);
    if (r < size) assert(r >= off && test_bit(r, a));
    r = find_first_zero_bit(a, size);
    if (r < size) assert(!test_bit(r, a));
    r = find_next_zero_bit(a, size, off);
    if (r < size) assert(r >= off && !test_bit(r, a));
    r = _find_first_bit(a, size) + _find_next_bit(a, size, off) + _find_first_zero_bit(a, size) +
        _find_next_zero_bit(a, size, off);
    (void)r;
}

/* Planted twin: a bitmap of one word searched as two (reads past it). */
extern "C" unsigned long kmodel_find_oob_bad(unsigned long w0) {
    unsigned long a[1] = {w0};
    return find_first_bit(a, 128);
}

/* ---- integer helpers ---- */

extern "C" void kmodel_ilog2_contract(unsigned n32, unsigned long long n64) {
    int l = __ilog2_u32(n32);
    if (n32 == 0) assert(l == -1);
    else assert(0 <= l && l <= 31 && (1u << l) <= n32 && (l == 31 || n32 < (1u << (l + 1))));
    int m = __ilog2_u64(n64);
    if (n64 == 0) assert(m == -1);
    else assert(0 <= m && m <= 63 && (1ull << m) <= n64 && (m == 63 || n64 < (1ull << (m + 1))));
}

extern "C" void kmodel_roundup_contract(unsigned long n) {
    if (n == 0 || n > (1UL << 63)) return;
    unsigned long r = __roundup_pow_of_two(n);
    assert((r & (r - 1)) == 0 && n <= r && (r == 1 || r / 2 < n));
}

/* 64-bit multiply/divide identity: the solvers time out on it (gap list). */
extern "C" void kmodel_div_contract(unsigned long long n64, unsigned d) {
    if (d == 0) return;
    unsigned rem = 0;
    unsigned long long q = div_u64_rem(n64, d, &rem);
    assert(rem < d && q * d + rem == n64 && div_u64(n64, d) == q);
}

/* Planted twin: __roundup_pow_of_two(0) (the kernel shifts by 64: UB). */
extern "C" unsigned long kmodel_roundup_zero_bad(unsigned long n) {
    if (n > 1) return 0;
    return __roundup_pow_of_two(n);
}

/* ---- reference counts ---- */

extern "C" void kmodel_refcount_contract(int start, int gets) {
    refcount_t r;
    kref k;
    if (start < 1 || start > 2 || gets < 0 || gets > 2) return;
    refcount_set(&r, start);
    assert(refcount_read(&r) == (unsigned)start);
    refcount_inc(&r);
    assert(refcount_read(&r) == (unsigned)start + 1);
    for (int i = 0; i <= start; i++) {
        bool last = refcount_dec_and_test(&r);
        assert(last == (i == start));
    }
    refcount_inc(&r); /* from 0: saturates, never 1 */
    assert(r.refs.counter == PRISM_REFCOUNT_SATURATED && !refcount_dec_and_test(&r));
    kref_init(&k);
    for (int i = 0; i < gets; i++) kref_get(&k);
    assert(k.refcount.refs.counter == 1 + gets);
    atomic_t a;
    atomic_set(&a, start);
    atomic_inc(&a);
    atomic_dec(&a);
    assert(atomic_read(&a) == start && !atomic_dec_and_test(&a) == (start != 1));
}

/* ---- allocation ---- */

extern "C" void kmodel_alloc_contract(unsigned n, unsigned j) {
    if (n == 0 || n > 8 || j >= n) return;
    unsigned char *z = (unsigned char *)kzalloc(n, 0);
    if (z) assert(z[j] == 0);
    unsigned char *m = (unsigned char *)kmalloc_array(n, 2, 0);
    if (m) m[2 * j + 1] = 1;
    unsigned char *c = (unsigned char *)kcalloc(n, 1, 0);
    if (c) assert(c[j] == 0);
    assert(kmalloc_array((size_t)-1, 2, 0) == 0); /* overflow: NULL */
    kfree(z);
    kfree(m);
    kfree(c);
    kfree(0);
    void *a = kmalloc(n, 0), *b = kzalloc_noprof(n, 0), *e = kmalloc_noprof(n, 0), *f = kmalloc_array_noprof(n, 1, 0);
    kfree(a);
    kfree(b);
    kfree(e);
    kfree(f);
}

/* Planted twin: kzalloc's NULL not checked. */
extern "C" unsigned char kmodel_kzalloc_null_bad(void) {
    unsigned char *p = (unsigned char *)kzalloc(4, 0);
    return p[0];
}

/* Planted twin: kfree twice. */
extern "C" void kmodel_kfree_twice_bad(void) {
    void *p = kmalloc(4, 0);
    kfree(p);
    kfree(p);
}

/* ---- strings, checksums ---- */

extern "C" void kmodel_strscpy_contract(int k, int size) {
    char src[4], dst[4];
    for (int i = 0; i < 4; i++) {
        char c = (char)__VERIFIER_nondet_int();
        src[i] = c ? c : 'q';
    }
    if (k < 0 || k >= 4 || size < 0 || size > 4) return;
    src[k] = 0;
    ssize_t r = strscpy(dst, src, (size_t)size);
    if (size == 0) assert(r == -7);
    else if (k < size) assert(r == k && dst[k] == 0);
    else assert(r == -7 && dst[size - 1] == 0);
}

extern "C" unsigned kmodel_crc32_le_contract(unsigned crc, unsigned len) {
    unsigned char b[4] = {1, 2, 3, 4};
    if (len > 4) return 0;
    return crc32_le(crc, b, len);
}

/* Planted twin: crc32_le over more bytes than the buffer has. */
extern "C" unsigned kmodel_crc32_le_oob_bad(unsigned crc) {
    unsigned char b[4] = {1, 2, 3, 4};
    return crc32_le(crc, b, 5);
}
