/* Bitmaps over unsigned long words (kernel include/linux/bitops.h,
 * lib/find_bit.c shapes): __set_bit, __clear_bit, test_bit,
 * find_first_bit, find_next_bit, find_first_zero_bit. The search loops step
 * a word at a time and take the lowest set bit with ctz, as the kernel does.
 * The bitmaps here are two words (128 bits): a size-bounded proof. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
unsigned long __VERIFIER_nondet_ulong(void);
/* --- end prelude --- */

#define BITS_PER_LONG 64UL
#define BITMAP_WORDS 2UL
#define BITMAP_BITS (BITMAP_WORDS * BITS_PER_LONG)
#define BIT_WORD(nr) ((nr) / BITS_PER_LONG)
#define BIT_MASK(nr) (1UL << ((nr) % BITS_PER_LONG))
#define OTHER_WORD(nr) (1UL - BIT_WORD(nr))

/*@ requires \valid(addr + (0..1));
    requires nr < BITMAP_BITS;
    assigns addr[BIT_WORD(nr)];
    ensures (addr[BIT_WORD(nr)] & BIT_MASK(nr)) != 0;
    ensures (addr[BIT_WORD(nr)] | BIT_MASK(nr)) == (\old(addr[BIT_WORD(nr)]) | BIT_MASK(nr));
    ensures addr[OTHER_WORD(nr)] == \old(addr[OTHER_WORD(nr)]);
*/
void __set_bit(unsigned long nr, unsigned long *addr) { addr[BIT_WORD(nr)] |= BIT_MASK(nr); }

/*@ requires \valid(addr + (0..1));
    requires nr < BITMAP_BITS;
    assigns addr[BIT_WORD(nr)];
    ensures (addr[BIT_WORD(nr)] & BIT_MASK(nr)) == 0;
    ensures (addr[BIT_WORD(nr)] | BIT_MASK(nr)) == (\old(addr[BIT_WORD(nr)]) | BIT_MASK(nr));
    ensures addr[OTHER_WORD(nr)] == \old(addr[OTHER_WORD(nr)]);
*/
void __clear_bit(unsigned long nr, unsigned long *addr) { addr[BIT_WORD(nr)] &= ~BIT_MASK(nr); }

/*@ requires \valid_read(addr + (0..1));
    requires nr < BITMAP_BITS;
    assigns \nothing;
    ensures \result == ((addr[BIT_WORD(nr)] & BIT_MASK(nr)) != 0);
*/
int test_bit(unsigned long nr, const unsigned long *addr) { return (int)((addr[BIT_WORD(nr)] >> (nr % BITS_PER_LONG)) & 1UL); }

/*@ requires \valid_read(addr + (0..1));
    requires size <= BITMAP_BITS;
    assigns \nothing;
    ensures \result <= size;
    ensures \result < size ==> ((addr[BIT_WORD(\result)] & BIT_MASK(\result)) != 0);
*/
unsigned long find_first_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long idx = 0; idx * BITS_PER_LONG < size; idx++) {
        unsigned long w = addr[idx];
        if (w) {
            unsigned long r = idx * BITS_PER_LONG + (unsigned long)__builtin_ctzl(w);
            return r < size ? r : size;
        }
    }
    return size;
}

/*@ requires \valid_read(addr + (0..1));
    requires size <= BITMAP_BITS;
    assigns \nothing;
    ensures \result <= size;
    ensures \result < size ==> \result >= offset && ((addr[BIT_WORD(\result)] & BIT_MASK(\result)) != 0);
*/
unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
    if (offset >= size) return size;
    unsigned long idx = offset / BITS_PER_LONG;
    unsigned long w = addr[idx] & (~0UL << (offset % BITS_PER_LONG));
    for (;;) {
        if (w) {
            unsigned long r = idx * BITS_PER_LONG + (unsigned long)__builtin_ctzl(w);
            return r < size ? r : size;
        }
        idx++;
        if (idx * BITS_PER_LONG >= size) return size;
        w = addr[idx];
    }
}

/*@ requires \valid_read(addr + (0..1));
    requires size <= BITMAP_BITS;
    assigns \nothing;
    ensures \result <= size;
    ensures \result < size ==> ((addr[BIT_WORD(\result)] & BIT_MASK(\result)) == 0);
*/
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long idx = 0; idx * BITS_PER_LONG < size; idx++) {
        unsigned long w = ~addr[idx];
        if (w) {
            unsigned long r = idx * BITS_PER_LONG + (unsigned long)__builtin_ctzl(w);
            return r < size ? r : size;
        }
    }
    return size;
}

/* ---- harnesses: the parts with a quantifier (every bit before the answer
 * is clear / set), stated for a symbolic bit j ---- */

#define TBIT(a, j) (((a)[BIT_WORD(j)] >> ((j) % BITS_PER_LONG)) & 1UL)

void find_first_bit_contract(unsigned long w0, unsigned long w1, unsigned long size, unsigned long j) {
    unsigned long a[2] = {w0, w1};
    if (size > BITMAP_BITS) return;
    unsigned long r = find_first_bit(a, size);
    assert(r <= size);
    if (r < size) assert(TBIT(a, r));
    if (j < r) assert(!TBIT(a, j)); /* every bit below the answer is clear */
}

void find_next_bit_contract(unsigned long w0, unsigned long w1, unsigned long size, unsigned long offset,
                            unsigned long j) {
    unsigned long a[2] = {w0, w1};
    if (size > BITMAP_BITS) return;
    unsigned long r = find_next_bit(a, size, offset);
    assert(r <= size);
    if (r < size) assert(r >= offset && TBIT(a, r));
    if (offset <= j && j < r) assert(!TBIT(a, j));
}

void find_first_zero_bit_contract(unsigned long w0, unsigned long w1, unsigned long size, unsigned long j) {
    unsigned long a[2] = {w0, w1};
    if (size > BITMAP_BITS) return;
    unsigned long r = find_first_zero_bit(a, size);
    assert(r <= size);
    if (r < size) assert(!TBIT(a, r));
    if (j < r) assert(TBIT(a, j));
}

/* set then test, clear then test, and the other bits untouched */
void set_clear_contract(unsigned long w0, unsigned long w1, unsigned long nr, unsigned long j) {
    unsigned long a[2] = {w0, w1};
    if (nr >= BITMAP_BITS || j >= BITMAP_BITS) return;
    unsigned long before = TBIT(a, j);
    __set_bit(nr, a);
    assert(test_bit(nr, a) == 1);
    if (j != nr) assert(TBIT(a, j) == before);
    __clear_bit(nr, a);
    assert(test_bit(nr, a) == 0);
    if (j != nr) assert(TBIT(a, j) == before);
}

/* Planted twin: "find_first_bit never returns size" (an empty bitmap). */
void find_first_bit_bad(unsigned long w0, unsigned long w1, unsigned long size) {
    unsigned long a[2] = {w0, w1};
    if (size > BITMAP_BITS) return;
    assert(find_first_bit(a, size) < size);
}

/* Planted twin: a bit number one past the bitmap (OOB). */
void set_bit_oob_bad(unsigned long nr) {
    unsigned long a[2] = {0, 0};
    if (nr > BITMAP_BITS) return;
    __set_bit(nr, a);
}
