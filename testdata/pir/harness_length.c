/* PIR tasks: length pairing (--pir-harness --pir-length-pairing, docs/PIR.md
 * "Length pairing"). A pointer the code indexes under a bound by an integer
 * parameter (or, for a pointer field, an integer field of the same object)
 * points to that many elements: a named assumption, so the best verdict is
 * PROVED-ASSUMING. Without the bound in the code there is no pairing. */

typedef unsigned long size_t;
typedef unsigned char u8;
typedef unsigned int u32;

/* (buf, n): every access is under i < n */
unsigned sum_buf(const int *buf, int n) {
    unsigned s = 0;
    for (int i = 0; i < n; i++) s += (unsigned)buf[i];
    return s;
}

/* off by one: i <= n reads buf[n], one past the paired elements (FAILED) */
unsigned sum_incl(const int *buf, int n) {
    unsigned s = 0;
    for (int i = 0; i <= n; i++) s += (unsigned)buf[i];
    return s;
}

/* the bound is checked, then the next element is read (FAILED at i = n - 1) */
unsigned sum_next(const int *buf, int n) {
    unsigned s = 0;
    for (int i = 0; i < n; i++) s += (unsigned)buf[i] + (unsigned)buf[i + 1];
    return s;
}

/* (const void *data, size_t size) with a typed view: size counts u32 elements */
u32 csum_words(const void *data, size_t size) {
    const u32 *w = data;
    u32 c = 0;
    for (size_t i = 0; i < size; i++) c ^= w[i];
    return c;
}

/* advanced pointers: while (len--) *dst++ = *src++ (both paired with len) */
void copy_bytes(u8 *dst, const u8 *src, size_t len) {
    while (len--) *dst++ = *src++;
}

/* an index check against the length parameter */
int pick_checked(const int *buf, int len, int idx) {
    if (idx >= 0 && idx < len) return buf[idx];
    return -1;
}

/* a length parameter that the code never compares the index with: no pairing */
int pick_unchecked(const int *buf, int len, int idx) {
    (void)len;
    return buf[idx];
}

/* the comparison the wrong way round: the access is not on the in-bound edge */
int pick_inverted(const int *buf, int len, int idx) {
    if (idx >= len) return buf[idx];
    return 0;
}

/* two different counts bound the same pointer: ambiguous, no pairing */
int two_bounds(const int *buf, int n, int m, int i) {
    int s = 0;
    if (i >= 0 && i < n) s += buf[i];
    if (i >= 0 && i < m) s += buf[i];
    return s;
}

/* passed through a helper together with its count */
static unsigned helper_sum(const short *p, int k) {
    unsigned s = 0;
    for (int j = 0; j < k; j++) s += (unsigned)p[j];
    return s;
}
unsigned via_helper(const short *v, int cnt) { return helper_sum(v, cnt); }

/* struct fields: a pointer field and a count field of the same object */
struct ring {
    int *slots;
    u32 n;
    u32 head;
};

int ring_get(struct ring *r, u32 i) {
    if (i < r->n) return r->slots[i];
    return -1;
}

int ring_get_incl(struct ring *r, u32 i) {
    if (i <= r->n) return r->slots[i];
    return -1;
}

/* the head field is not compared with anything: no pairing for slots[head] */
int ring_head(struct ring *r) { return r->slots[r->head]; }

/* library calls with an explicit length (--pir-length-pairing): the pointer and the length argument */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

int lib_eq(const u8 *a, const u8 *b, size_t len) { return memcmp(a, b, len); }
void lib_copy(u8 *dst, const u8 *src, size_t n) { memcpy(dst, src, n); }
void lib_zero(u8 *p, size_t n) { memset(p, 0, n); }

/* off by one after the library call: p[n] is one past the n bytes the pairing gives (FAILED) */
void lib_zero_term(u8 *p, size_t n) {
    memset(p, 0, n);
    p[n] = 0;
}

/* the length is not the parameter itself: no pairing */
void lib_copy_plus(u8 *dst, const u8 *src, size_t n) { memcpy(dst, src, n + 1); }
