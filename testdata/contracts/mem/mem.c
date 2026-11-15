/* Byte loops: memset, memcpy, memcmp, memchr, strlen and strnlen shapes,
 * written as plain loops (what a kernel or embedded tree has instead of the
 * C library). The per-byte parts of a contract (every byte set, copied,
 * scanned) are quantified; they are stated by the harnesses for objects of
 * up to MN bytes, the rest by the ACSL contracts for any length the
 * unwinding (or a loop invariant) covers. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
#define ND_U8() ((unsigned char)__VERIFIER_nondet_int())
#define FILL_U8(b, cap) do { for (int i_ = 0; i_ < (int)(cap); i_++) (b)[i_] = ND_U8(); } while (0)
/* --- end prelude --- */

#define MN 4

/*@ requires \valid(p + (0..n-1));
    requires 0 <= n <= 16;
    assigns p[0..n-1];
    ensures \result == p;
*/
unsigned char *bmemset(unsigned char *p, int c, int n) {
    for (int i = 0; i < n; i++) p[i] = (unsigned char)c;
    return p;
}

/*@ requires \valid(d + (0..n-1)) && \valid_read(s + (0..n-1));
    requires 0 <= n <= 16;
    assigns d[0..n-1];
    ensures \result == d;
*/
unsigned char *bmemcpy(unsigned char *d, const unsigned char *s, int n) {
    for (int i = 0; i < n; i++) d[i] = s[i];
    return d;
}

/*@ requires \valid_read(a + (0..n-1)) && \valid_read(b + (0..n-1));
    requires 0 <= n <= 16;
    assigns \nothing;
    ensures -1 <= \result <= 1;
*/
int bmemcmp(const unsigned char *a, const unsigned char *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* Index of the first byte equal to c, or -1. */
/*@ requires \valid_read(p + (0..n-1));
    requires 0 <= n <= 16;
    assigns \nothing;
    ensures -1 <= \result < n;
    ensures \result >= 0 ==> p[\result] == (unsigned char)c;
*/
int bmemchr(const unsigned char *p, int c, int n) {
    for (int i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return i;
    return -1;
}

/* Length of the string in s[0..cap-1]; cap when unterminated (a bounded
 * scan never reads past cap). */
/*@ requires \valid_read(s + (0..cap-1));
    requires 0 <= cap <= 16;
    assigns \nothing;
    ensures 0 <= \result <= cap;
    ensures \result < cap ==> s[\result] == 0;
*/
int bstrnlen(const char *s, int cap) {
    int i = 0;
    while (i < cap && s[i]) i++;
    return i;
}

/* ---- harnesses ---- */

void bmemset_contract(int c, int n, int j) {
    unsigned char p[MN];
    FILL_U8(p, MN);
    unsigned char keep = p[j >= 0 && j < MN ? j : 0];
    if (n < 0 || n > MN || j < 0 || j >= MN) return;
    bmemset(p, c, n);
    if (j < n) assert(p[j] == (unsigned char)c);
    else assert(p[j] == keep);
}

void bmemcpy_contract(int n, int j) {
    unsigned char d[MN], s[MN];
    FILL_U8(d, MN);
    FILL_U8(s, MN);
    if (n < 0 || n > MN || j < 0 || j >= MN) return;
    unsigned char keep = d[j];
    bmemcpy(d, s, n);
    if (j < n) assert(d[j] == s[j]);
    else assert(d[j] == keep);
}

void bmemcmp_contract(int n, int j) {
    unsigned char a[MN], b[MN];
    FILL_U8(a, MN);
    if (n < 0 || n > MN) return;
    for (int i = 0; i < MN; i++) b[i] = a[i];
    assert(bmemcmp(a, b, n) == 0);
    if (j < 0 || j >= n || a[j] == 255) return;
    b[j] = (unsigned char)(a[j] + 1); /* first difference at j, b larger */
    assert(bmemcmp(a, b, n) == -1 && bmemcmp(b, a, n) == 1);
}

void bmemchr_contract(int c, int n, int j) {
    unsigned char p[MN];
    FILL_U8(p, MN);
    if (n < 0 || n > MN) return;
    int r = bmemchr(p, c, n);
    if (r >= 0) assert(p[r] == (unsigned char)c);
    if (0 <= j && j < n && (r < 0 || j < r)) assert(p[j] != (unsigned char)c);
}

void bstrnlen_contract(int k, int cap) {
    char s[MN];
    for (int i = 0; i < MN; i++) {
        char c = (char)__VERIFIER_nondet_int();
        s[i] = c ? c : 'x';
    }
    if (cap < 0 || cap > MN || k < 0 || k >= MN) return;
    s[k] = 0;
    int r = bstrnlen(s, cap);
    assert(r == (k < cap ? k : cap));
}

/* Planted twin: memset with n one past the object. */
void bmemset_oob_bad(int n) {
    unsigned char p[MN];
    if (n < 0 || n > MN + 1) return;
    bmemset(p, 0, n);
}

/* Planted twin: an unbounded strlen over an unterminated array. */
int bstrlen_unterminated_bad(void) {
    char s[MN];
    for (int i = 0; i < MN; i++) s[i] = 'a';
    int i = 0;
    while (s[i]) i++;
    return i;
}
