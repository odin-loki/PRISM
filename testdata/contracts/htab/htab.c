/* Hash tables: open addressing with linear probing over a fixed table, and
 * chaining with bucket heads and next indices in a node array. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
unsigned __VERIFIER_nondet_uint(void);
/* --- end prelude --- */

#define HT_N 4u /* slots, a power of two */

struct oatab {
    unsigned keys[HT_N];
    int used[HT_N];
};

/*@ assigns \nothing;
    ensures \result < HT_N;
*/
unsigned ht_slot(unsigned key) { return (key * 2654435761u) & (HT_N - 1u); }

/* Lookup: the slot holding key, or -1 when a probe of every slot finds it
 * nowhere (or meets an empty slot first). */
/*@ requires \valid_read(t);
    assigns \nothing;
    ensures -1 <= \result < (int)HT_N;
    ensures \result >= 0 ==> t->used[\result] && t->keys[\result] == key;
*/
int ht_lookup(const struct oatab *t, unsigned key) {
    unsigned h = ht_slot(key);
    for (unsigned i = 0; i < HT_N; i++) {
        unsigned s = (h + i) & (HT_N - 1u);
        if (!t->used[s]) return -1;
        if (t->keys[s] == key) return (int)s;
    }
    return -1;
}

/* Insert or find: the slot of key afterwards, or -1 when the table is full. */
/*@ requires \valid(t);
    assigns t->keys[0..HT_N-1], t->used[0..HT_N-1];
    ensures -1 <= \result < (int)HT_N;
    ensures \result >= 0 ==> t->used[\result] && t->keys[\result] == key;
*/
int ht_insert(struct oatab *t, unsigned key) {
    unsigned h = ht_slot(key);
    for (unsigned i = 0; i < HT_N; i++) {
        unsigned s = (h + i) & (HT_N - 1u);
        if (!t->used[s]) {
            t->used[s] = 1;
            t->keys[s] = key;
            return (int)s;
        }
        if (t->keys[s] == key) return (int)s;
    }
    return -1;
}

/* insert then lookup finds the key (no deletions: probing never stops early
 * at a hole left by a delete) */
void ht_insert_lookup_contract(unsigned a, unsigned b, unsigned k) {
    struct oatab t;
    for (unsigned i = 0; i < HT_N; i++) t.used[i] = 0;
    assert(ht_insert(&t, a) >= 0);
    assert(ht_insert(&t, b) >= 0);
    int s = ht_insert(&t, k);
    assert(s >= 0);
    assert(ht_lookup(&t, k) == s);
    assert(ht_lookup(&t, a) >= 0 && ht_lookup(&t, b) >= 0);
}

/* Planted twin: "a lookup of a key never inserted fails" is wrong when the
 * table has the key from another insert (here: the same value). */
void ht_lookup_absent_bad(unsigned a, unsigned k) {
    struct oatab t;
    for (unsigned i = 0; i < HT_N; i++) t.used[i] = 0;
    ht_insert(&t, a);
    assert(ht_lookup(&t, k) == -1);
}

/* ---- chaining ---- */

#define CH_BUCKETS 2
#define CH_NODES 4

struct chtab {
    int head[CH_BUCKETS];   /* first node index of each bucket, -1: empty */
    int next[CH_NODES];     /* next node index in the chain, -1: end */
    unsigned key[CH_NODES];
    int nfree;              /* nodes 0..nfree-1 are in use */
};

/*@ requires \valid_read(t);
    requires 0 <= t->nfree <= CH_NODES;
    assigns \nothing;
    ensures -1 <= \result < CH_NODES;
    ensures \result >= 0 ==> t->key[\result] == key;
*/
int ch_lookup(const struct chtab *t, unsigned key) {
    int n = t->head[key % CH_BUCKETS];
    for (int steps = 0; steps < CH_NODES && n >= 0 && n < CH_NODES; steps++) {
        if (t->key[n] == key) return n;
        n = t->next[n];
    }
    return -1;
}

/* Push a new node at the head of its bucket; -1 when no node is free. */
/*@ requires \valid(t);
    requires 0 <= t->nfree <= CH_NODES;
    assigns t->head[0..CH_BUCKETS-1], t->next[0..CH_NODES-1], t->key[0..CH_NODES-1], t->nfree;
    ensures \result == -1 <==> \old(t->nfree) == CH_NODES;
    ensures \result >= 0 ==> \result == \old(t->nfree) && t->key[\result] == key && t->head[key % CH_BUCKETS] == \result;
    ensures 0 <= t->nfree <= CH_NODES;
*/
int ch_insert(struct chtab *t, unsigned key) {
    if (t->nfree >= CH_NODES) return -1;
    int n = t->nfree++;
    t->key[n] = key;
    t->next[n] = t->head[key % CH_BUCKETS];
    t->head[key % CH_BUCKETS] = n;
    return n;
}

void ch_contract(unsigned a, unsigned b, unsigned c) {
    struct chtab t;
    t.head[0] = t.head[1] = -1;
    t.nfree = 0;
    assert(ch_insert(&t, a) == 0);
    assert(ch_insert(&t, b) == 1);
    assert(ch_insert(&t, c) == 2);
    assert(t.key[ch_lookup(&t, a)] == a);
    assert(ch_lookup(&t, b) >= 0 && ch_lookup(&t, c) >= 0);
}
