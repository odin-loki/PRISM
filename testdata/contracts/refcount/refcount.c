/* Reference counts (kernel include/linux/refcount.h and kref.h shapes): a
 * saturating counter (an increment at the maximum or from zero saturates
 * instead of wrapping, so the object is leaked rather than freed early),
 * dec_and_test that reports the last put, and a get/put pair around a
 * release. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
unsigned __VERIFIER_nondet_uint(void);
/* --- end prelude --- */

#define REFCOUNT_SATURATED 0xC0000000u

typedef struct {
    unsigned refs;
} refcount_t;

struct kref {
    refcount_t refcount;
};

/*@ requires \valid(r);
    assigns r->refs;
    ensures r->refs == n;
*/
void refcount_set(refcount_t *r, unsigned n) { r->refs = n; }

/*@ requires \valid_read(r);
    assigns \nothing;
    ensures \result == r->refs;
*/
unsigned refcount_read(const refcount_t *r) { return r->refs; }

/* An increment of 0 (use after free) or of a counter at or past the
 * saturation point leaves it saturated; otherwise it adds one. */
/*@ requires \valid(r);
    assigns r->refs;
    ensures \old(r->refs) == 0 || \old(r->refs) >= REFCOUNT_SATURATED - 1 ==> r->refs == REFCOUNT_SATURATED;
    ensures 0 < \old(r->refs) < REFCOUNT_SATURATED - 1 ==> r->refs == \old(r->refs) + 1;
*/
void refcount_inc(refcount_t *r) {
    unsigned old = r->refs;
    if (old == 0 || old >= REFCOUNT_SATURATED - 1) {
        r->refs = REFCOUNT_SATURATED; /* the kernel also warns once */
        return;
    }
    r->refs = old + 1;
}

/* True exactly when this put dropped the last reference. A saturated
 * counter stays saturated (never reaches 0); a put of 0 is a bug (stays 0,
 * returns false). */
/*@ requires \valid(r);
    assigns r->refs;
    ensures \result == 1 <==> \old(r->refs) == 1;
    ensures \result == 1 ==> r->refs == 0;
    ensures 1 < \old(r->refs) < REFCOUNT_SATURATED ==> r->refs == \old(r->refs) - 1;
    ensures \old(r->refs) >= REFCOUNT_SATURATED ==> r->refs == REFCOUNT_SATURATED;
    ensures \old(r->refs) == 0 ==> r->refs == 0 && \result == 0;
*/
int refcount_dec_and_test(refcount_t *r) {
    unsigned old = r->refs;
    if (old >= REFCOUNT_SATURATED) {
        r->refs = REFCOUNT_SATURATED;
        return 0;
    }
    if (old == 0) return 0;
    r->refs = old - 1;
    return old == 1;
}

/*@ requires \valid(k);
    assigns k->refcount.refs;
    ensures k->refcount.refs == 1;
*/
void kref_init(struct kref *k) { refcount_set(&k->refcount, 1); }

/*@ requires \valid(k);
    requires 0 < k->refcount.refs && k->refcount.refs < REFCOUNT_SATURATED - 1;
    assigns k->refcount.refs;
    ensures k->refcount.refs == \old(k->refcount.refs) + 1;
*/
void kref_get(struct kref *k) { refcount_inc(&k->refcount); }

/* kref_put with the release folded in as a flag (the kernel passes a
 * function pointer): returns 1 when the release ran. */
/*@ requires \valid(k) && \valid(released);
    requires 0 < k->refcount.refs && k->refcount.refs < REFCOUNT_SATURATED;
    assigns k->refcount.refs, *released;
    ensures \result == 1 <==> \old(k->refcount.refs) == 1;
    ensures \result == 1 ==> *released == 1;
    ensures \result == 0 ==> *released == \old(*released);
*/
int kref_put(struct kref *k, int *released) {
    if (refcount_dec_and_test(&k->refcount)) {
        *released = 1;
        return 1;
    }
    return 0;
}

/* get/put balance: n gets then n + 1 puts release exactly once, at the last. */
void kref_balance_contract(int n) {
    struct kref k;
    int released = 0;
    if (n < 0 || n > 3) return;
    kref_init(&k);
    for (int i = 0; i < n; i++) kref_get(&k);
    for (int i = 0; i < n; i++) {
        assert(kref_put(&k, &released) == 0);
        assert(released == 0);
    }
    assert(kref_put(&k, &released) == 1 && released == 1 && k.refcount.refs == 0);
}

/* Planted twin of a contract: "inc always adds one" (wrong at saturation). */
/*@ requires \valid(r);
    ensures r->refs == \old(r->refs) + 1;
*/
void refcount_inc_adds_one_bad(refcount_t *r) { refcount_inc(r); }
