/* PRISM hard benchmark: ring buffers with head/tail indices and pointers.
 * See README.md. */
#include <stdint.h>

#define RING 16

struct ring {
    unsigned char data[RING];
    unsigned head, tail, count;
};

/* Push/pop driven by an op array on a local ring; indices wrap with % RING. */
// requires: \valid_read(ops + (0..n-1))
int ring_ops_ok(const unsigned char *ops, int n) {
    struct ring r = {{0}, 0, 0, 0};
    if (n < 0 || n > 40) return -1;
    int out = 0;
    for (int i = 0; i < n; i++) {
        if (ops[i] & 1) {
            if (r.count < RING) {
                r.data[r.head] = ops[i];
                r.head = (r.head + 1) % RING;
                r.count++;
            }
        } else if (r.count > 0) {
            out += r.data[r.tail];
            r.tail = (r.tail + 1) % RING;
            r.count--;
        }
    }
    return out + (int)r.count;
}

/* Planted bug: head is never wrapped, so a push after 16 pushes and one pop
 * writes data[16]. */
// requires: \valid_read(ops + (0..n-1))
int ring_nowrap_bad(const unsigned char *ops, int n) {
    struct ring r = {{0}, 0, 0, 0};
    if (n < 0 || n > 24) return -1;
    for (int i = 0; i < n; i++) {
        if (ops[i] & 1) {
            if (r.count < RING) {
                r.data[r.head] = ops[i];
                r.head = r.head + 1;              /* no wrap: data[16] after a pop */
                r.count++;
            }
        } else if (r.count > 0) {
            r.tail = (r.tail + 1) % RING;
            r.count--;
        }
    }
    return (int)r.count;
}

/* Ring passed by pointer; one push. */
// requires: \valid(r)
int ring_push_ok(struct ring *r, unsigned char b) {
    if (r->head >= RING || r->count >= RING) return -1;
    r->data[r->head] = b;
    r->head = (r->head + 1) % RING;
    r->count++;
    return 0;
}

/* Drain n bytes from a caller ring into a caller buffer. */
// requires: \valid(r)
// requires: \valid(dst + (0..n-1))
int ring_drain_ok(struct ring *r, unsigned char *dst, int n) {
    if (n < 0 || n > 20) return -1;
    if (r->tail >= RING || r->count > RING) return -1;
    int k = 0;
    while (k < n && r->count > 0) {
        dst[k] = r->data[r->tail];
        r->tail = (r->tail + 1) % RING;
        r->count--;
        k++;
    }
    return k;
}

/* Drain into dst with the bound check against the wrong length. */
// requires: \valid(r)
// requires: \valid(dst + (0..n-1))
int ring_drain_bad(struct ring *r, unsigned char *dst, int n) {
    if (n < 0 || n > 20) return -1;
    if (r->tail >= RING || r->count > RING) return -1;
    int k = 0;
    while (k <= n && r->count > 0) {          /* k == n writes dst[n] */
        dst[k] = r->data[r->tail];
        r->tail = (r->tail + 1) % RING;
        r->count--;
        k++;
    }
    return k;
}

/* Power-of-two ring with 64-bit free-running counters and a mask: the
 * difference head - tail is computed in unsigned 64-bit arithmetic. */
uint64_t ring_counters_ok(uint64_t head, uint64_t tail, int pushes) {
    uint64_t buf[32];
    if (head - tail > 32 || pushes < 0 || pushes > 64) return 0;
    for (int i = 0; i < 32; i++) buf[i] = (uint64_t)i * 0x9e3779b97f4a7c15ull;
    uint64_t acc = 0;
    for (int i = 0; i < pushes; i++) {
        if (head - tail < 32) {
            buf[head & 31] ^= acc;
            head++;
        } else {
            acc += buf[tail & 31] >> 3;
            tail++;
        }
    }
    return acc + (head - tail);
}

/* 64-bit average of the ring's slots: the sum overflows (signed). */
int64_t ring_avg_bad(int64_t seed) {
    int64_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = seed + i;     /* seed + i overflows */
    int64_t s = 0;
    for (int i = 0; i < 64; i++) s += buf[i] >> 7;
    return s / 64;
}

/* Fixed 128-step simulation of producer/consumer on a 16-slot ring. */
int ring_sim128_ok(unsigned pattern) {
    unsigned char data[RING];
    unsigned head = 0, tail = 0, count = 0, sum = 0;
    for (int i = 0; i < 128; i++) {
        if ((pattern >> (i & 31)) & 1) {
            if (count < RING) { data[head] = (unsigned char)i; head = (head + 1) & (RING - 1); count++; }
        } else if (count) {
            sum += data[tail]; tail = (tail + 1) & (RING - 1); count--;
        }
    }
    return (int)(sum & 0xffff) + (int)count;
}

/* Unbounded producer loop on pointers into a static ring: the slot index
 * stays in range (needs an invariant). */
static int g_ring[RING];
int ring_unbounded_ok(int n, int v) {
    int *p = g_ring;
    int *end = g_ring + RING;
    while (n > 0) {
        *p = v;
        p++;
        if (p == end) p = g_ring;
        n--;
    }
    return g_ring[0];
}
