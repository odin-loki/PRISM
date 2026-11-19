/* Fixed-size allocators: a free list threaded through an index array (the
 * slab / mempool free-list shape) and a bitmap of used slots with a first-fit
 * scan. Slots are indices into a static array of objects. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
/* --- end prelude --- */

#define POOL_N 4

struct pool {
    int head;           /* first free slot, -1 when none */
    int next[POOL_N];   /* next free slot after i, -1 at the end */
    int used[POOL_N];   /* 1 while slot i is allocated */
};

/*@ requires \valid(p);
    assigns p->head, p->next[0..POOL_N-1], p->used[0..POOL_N-1];
    ensures p->head == 0;
*/
void pool_init(struct pool *p) {
    for (int i = 0; i < POOL_N; i++) {
        p->next[i] = i + 1 < POOL_N ? i + 1 : -1;
        p->used[i] = 0;
    }
    p->head = 0;
}

/* Pop the head of the free list. */
/*@ requires \valid(p);
    requires p->head == -1 || (0 <= p->head < POOL_N && !p->used[p->head]);
    requires p->head == -1 || -1 <= p->next[p->head] < POOL_N;
    assigns p->head, p->used[0..POOL_N-1];
    ensures \result == \old(p->head);
    ensures \result != -1 ==> 0 <= \result < POOL_N && p->used[\result] == 1 && p->head == \old(p->head == -1 ? -1 : p->next[p->head]);
    ensures \result == -1 ==> p->head == -1;
*/
int pool_alloc(struct pool *p) {
    int i = p->head;
    if (i < 0) return -1;
    p->head = p->next[i];
    p->used[i] = 1;
    return i;
}

/* Push slot i back; freeing a free slot or a slot out of range is refused. */
/*@ requires \valid(p);
    requires -1 <= p->head < POOL_N;
    assigns p->head, p->next[0..POOL_N-1], p->used[0..POOL_N-1];
    ensures \result == 0 || \result == -1;
    ensures \result == 0 <==> (0 <= i < POOL_N && \old(0 <= i && i < POOL_N ? p->used[i] : 0) != 0);
    ensures \result == 0 ==> p->head == i && p->used[i] == 0 && p->next[i] == \old(p->head);
    ensures \result == -1 ==> p->head == \old(p->head);
*/
int pool_free(struct pool *p, int i) {
    if (i < 0 || i >= POOL_N || !p->used[i]) return -1;
    p->used[i] = 0;
    p->next[i] = p->head;
    p->head = i;
    return 0;
}

/* Bitmap first-fit allocator over a word of used bits. */
/*@ requires \valid(used);
    assigns *used;
    ensures -1 <= \result < 32;
    ensures \result >= 0 ==> (\old(*used) & (1u << \result)) == 0 && *used == (\old(*used) | (1u << \result));
    ensures \result == -1 ==> \old(*used) == 0xffffffffu && *used == \old(*used);
*/
int bitmap_alloc(unsigned *used) {
    for (int i = 0; i < 32; i++)
        if (!(*used & (1u << i))) {
            *used |= 1u << i;
            return i;
        }
    return -1;
}

/* Allocate everything, free in any order, allocate again: never the same
 * slot twice while it is in use, and exactly POOL_N slots exist. */
void pool_contract(int f0, int f1) {
    struct pool p;
    int got[POOL_N];
    pool_init(&p);
    for (int k = 0; k < POOL_N; k++) {
        got[k] = pool_alloc(&p);
        assert(got[k] >= 0 && got[k] < POOL_N);
        for (int j = 0; j < k; j++) assert(got[j] != got[k]);
    }
    assert(pool_alloc(&p) == -1);
    if (f0 < 0 || f0 >= POOL_N || f1 < 0 || f1 >= POOL_N) return;
    assert(pool_free(&p, f0) == 0);
    assert(pool_free(&p, f0) == -1); /* double free refused */
    int r = pool_alloc(&p);
    assert(r == f0);
    assert(pool_free(&p, f1) == 0);
    assert(pool_alloc(&p) == f1);
}

/* Planted twin of a contract: "alloc never fails". */
/*@ requires \valid(p);
    requires p->head == -1 || (0 <= p->head < POOL_N && !p->used[p->head]);
    requires p->head == -1 || -1 <= p->next[p->head] < POOL_N;
    ensures \result >= 0;
*/
int pool_alloc_never_fails_bad(struct pool *p) { return pool_alloc(p); }
