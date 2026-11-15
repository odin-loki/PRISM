/* Ring buffers: free-running head/tail with a power-of-two mask (kernel
 * kfifo / circ_buf style) and wrapped indices with a modulo and a count. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
unsigned __VERIFIER_nondet_uint(void);
#define ND_U8() ((unsigned char)__VERIFIER_nondet_int())
#define FILL_U8(b, cap) do { for (int i_ = 0; i_ < (int)(cap); i_++) (b)[i_] = ND_U8(); } while (0)
/* --- end prelude --- */

#define RING_SIZE 16u /* a power of two */
#define RING_MASK (RING_SIZE - 1u)

struct ring {
    unsigned head, tail; /* free-running; used = head - tail (mod 2^32) */
    unsigned char buf[RING_SIZE];
};

/*@ requires \valid_read(r);
    requires r->head - r->tail <= RING_SIZE;
    assigns \nothing;
    ensures \result == r->head - r->tail && \result <= RING_SIZE;
*/
unsigned ring_used(const struct ring *r) { return r->head - r->tail; }

/*@ requires \valid(r);
    requires r->head - r->tail <= RING_SIZE;
    assigns r->head, r->buf[0..RING_SIZE-1];
    ensures \result == 0 || \result == -1;
    ensures \result == 0 ==> r->head == \old(r->head) + 1 && r->buf[\old(r->head) & RING_MASK] == b;
    ensures \result == -1 ==> \old(r->head - r->tail) == RING_SIZE && r->head == \old(r->head);
    ensures r->tail == \old(r->tail) && r->head - r->tail <= RING_SIZE;
*/
int ring_push(struct ring *r, unsigned char b) {
    if (r->head - r->tail >= RING_SIZE) return -1;
    r->buf[r->head & RING_MASK] = b;
    r->head++;
    return 0;
}

/*@ requires \valid(r) && \valid(out);
    requires r->head - r->tail <= RING_SIZE;
    assigns r->tail, *out;
    ensures \result == 0 || \result == -1;
    ensures \result == 0 ==> *out == \old(r->buf[r->tail & RING_MASK]) && r->tail == \old(r->tail) + 1;
    ensures \result == -1 ==> \old(r->head) == \old(r->tail) && r->tail == \old(r->tail);
    ensures r->head == \old(r->head) && r->head - r->tail <= RING_SIZE;
*/
int ring_pop(struct ring *r, unsigned char *out) {
    if (r->head == r->tail) return -1;
    *out = r->buf[r->tail & RING_MASK];
    r->tail++;
    return 0;
}

/*@ requires \valid(r); */
static void ring_make(struct ring *r, unsigned head, unsigned tail) {
    r->head = head;
    r->tail = tail;
    FILL_U8(r->buf, (int)RING_SIZE);
}

void ring_used_contract(unsigned head, unsigned tail) {
    struct ring r;
    ring_make(&r, head, tail);
    if (!(head - tail <= RING_SIZE)) return;
    unsigned u = ring_used(&r);
    assert(u == head - tail && u <= RING_SIZE);
}

void ring_push_contract(unsigned head, unsigned tail, unsigned char b) {
    struct ring r;
    ring_make(&r, head, tail);
    if (!(head - tail <= RING_SIZE)) return;
    int rc = ring_push(&r, b);
    assert(rc == 0 || rc == -1);
    if (rc == 0) assert(r.head == head + 1 && r.buf[head & RING_MASK] == b);
    else assert(head - tail == RING_SIZE && r.head == head);
    assert(r.tail == tail && r.head - r.tail <= RING_SIZE);
}

void ring_pop_contract(unsigned head, unsigned tail) {
    struct ring r;
    unsigned char out = 0;
    ring_make(&r, head, tail);
    if (!(head - tail <= RING_SIZE)) return;
    unsigned char want = r.buf[tail & RING_MASK];
    int rc = ring_pop(&r, &out);
    assert(rc == 0 || rc == -1);
    if (rc == 0) assert(out == want && r.tail == tail + 1);
    else assert(head == tail && r.tail == tail);
    assert(r.head == head && r.head - r.tail <= RING_SIZE);
}

/* FIFO order: a byte pushed into an empty ring is the next byte popped. */
void ring_fifo_contract(unsigned start, unsigned char b) {
    struct ring r;
    unsigned char out = 0;
    ring_make(&r, start, start);
    int a = ring_push(&r, b);
    int c = ring_pop(&r, &out);
    assert(a == 0 && c == 0 && out == b && r.head == r.tail);
}

/* Planted twin: "push never fails" is wrong on a full ring. */
void ring_push_bad(unsigned head, unsigned tail, unsigned char b) {
    struct ring r;
    ring_make(&r, head, tail);
    if (!(head - tail <= RING_SIZE)) return;
    assert(ring_push(&r, b) == 0);
}

/* ---- wrapped indices with a modulo and a count (any capacity) ---- */

#define MRING_CAP 12u /* not a power of two */

struct mring {
    unsigned head, tail, count; /* head, tail < cap; count <= cap */
    unsigned char data[MRING_CAP];
};

/*@ requires \valid(r);
    requires r->head < MRING_CAP && r->tail < MRING_CAP && r->count <= MRING_CAP;
    requires (r->tail + r->count) % MRING_CAP == r->head;
    assigns r->head, r->count, r->data[0..MRING_CAP-1];
    ensures \result == 0 ==> r->count == \old(r->count) + 1 && r->data[\old(r->head)] == b;
    ensures \result == -1 ==> \old(r->count) == MRING_CAP;
    ensures r->head < MRING_CAP && r->count <= MRING_CAP && (r->tail + r->count) % MRING_CAP == r->head;
*/
int mring_push(struct mring *r, unsigned char b) {
    if (r->count >= MRING_CAP) return -1;
    r->data[r->head] = b;
    r->head = (r->head + 1) % MRING_CAP;
    r->count++;
    return 0;
}

/*@ requires \valid(r) && \valid(out);
    requires r->head < MRING_CAP && r->tail < MRING_CAP && r->count <= MRING_CAP;
    requires (r->tail + r->count) % MRING_CAP == r->head;
    assigns r->tail, r->count, *out;
    ensures \result == 0 ==> *out == \old(r->data[r->tail]) && r->count == \old(r->count) - 1;
    ensures \result == -1 ==> \old(r->count) == 0;
    ensures r->tail < MRING_CAP && r->count <= MRING_CAP && (r->tail + r->count) % MRING_CAP == r->head;
*/
int mring_pop(struct mring *r, unsigned char *out) {
    if (r->count == 0) return -1;
    *out = r->data[r->tail];
    r->tail = (r->tail + 1) % MRING_CAP;
    r->count--;
    return 0;
}

/*@ requires \valid_read(r); */
static int mring_ok(const struct mring *r) {
    return r->head < MRING_CAP && r->tail < MRING_CAP && r->count <= MRING_CAP &&
           (r->tail + r->count) % MRING_CAP == r->head;
}

void mring_push_contract(unsigned head, unsigned tail, unsigned count, unsigned char b) {
    struct mring r;
    r.head = head;
    r.tail = tail;
    r.count = count;
    FILL_U8(r.data, (int)MRING_CAP);
    if (!mring_ok(&r)) return;
    int rc = mring_push(&r, b);
    if (rc == 0) assert(r.count == count + 1 && r.data[head] == b);
    else assert(rc == -1 && count == MRING_CAP);
    assert(r.tail == tail && mring_ok(&r));
}

void mring_pop_contract(unsigned head, unsigned tail, unsigned count) {
    struct mring r;
    unsigned char out = 0;
    r.head = head;
    r.tail = tail;
    r.count = count;
    FILL_U8(r.data, (int)MRING_CAP);
    if (!mring_ok(&r)) return;
    unsigned char want = r.data[tail];
    int rc = mring_pop(&r, &out);
    if (rc == 0) assert(out == want && r.count == count - 1);
    else assert(rc == -1 && count == 0);
    assert(r.head == head && mring_ok(&r));
}

/* Planted twin: the modulo omitted, head reaches the capacity (OOB write on
 * the next push). */
void mring_nowrap_bad(unsigned head, unsigned tail, unsigned count, unsigned char b) {
    struct mring r;
    r.head = head;
    r.tail = tail;
    r.count = count;
    FILL_U8(r.data, (int)MRING_CAP);
    if (!mring_ok(&r)) return;
    if (r.count >= MRING_CAP) return;
    r.data[r.head] = b;
    r.head = r.head + 1; /* no wrap */
    r.count++;
    if (r.count < MRING_CAP) r.data[r.head] = b; /* head == MRING_CAP: out of bounds */
}

/* Planted twin of a contract: "push never fails" (a full ring refuses); the
 * contracts stage must refute it. */
/*@ requires \valid(r);
    requires r->head - r->tail <= RING_SIZE;
    ensures \result == 0;
*/
int ring_push_never_full_bad(struct ring *r, unsigned char b) { return ring_push(r, b); }
