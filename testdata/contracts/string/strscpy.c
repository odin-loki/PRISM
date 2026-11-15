/* Bounded string copy (kernel strscpy, BSD strlcpy shapes): never writes
 * past size bytes, always terminates the destination when size > 0. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
/* --- end prelude --- */

#define E2BIG 7
#define SZ 4

/* Copies at most size - 1 bytes and a NUL; returns the length copied, or
 * -E2BIG when src did not fit (dst then holds a truncated, terminated
 * prefix). src is read up to its NUL or size bytes. */
/*@ requires \valid(dst + (0..size-1)) && \valid_read(src + (0..size-1));
    requires 0 <= size <= 16;
    assigns dst[0..size-1];
    ensures \result == -E2BIG || 0 <= \result < size;
    ensures size == 0 ==> \result == -E2BIG;
    ensures \result >= 0 ==> dst[\result] == 0;
    ensures \result == -E2BIG && size > 0 ==> dst[size - 1] == 0;
*/
int strscpy(char *dst, const char *src, int size) {
    if (size <= 0) return -E2BIG;
    int i = 0;
    while (i < size) {
        char c = src[i];
        dst[i] = c;
        if (!c) return i;
        i++;
    }
    dst[size - 1] = 0;
    return -E2BIG;
}

/* strlcpy: returns strlen(src), copies min(len, size - 1). src must be a
 * string (here: terminated within its object of srcn bytes). */
/*@ requires \valid(dst + (0..size-1)) && \valid_read(src + (0..srcn-1));
    requires 1 <= size <= 16 && 1 <= srcn <= 16;
    assigns dst[0..size-1];
    ensures -1 <= \result < srcn;
    ensures \result >= 0 ==> src[\result] == 0;
*/
int bstrlcpy(char *dst, const char *src, int size, int srcn) {
    int len = 0;
    while (len < srcn && src[len]) len++;
    if (len == srcn) return -1; /* unterminated source: refused */
    int n = len < size - 1 ? len : size - 1;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = 0;
    return len;
}

void strscpy_contract(int k, int size, int j) {
    char src[SZ], dst[SZ];
    for (int i = 0; i < SZ; i++) {
        char c = (char)__VERIFIER_nondet_int();
        src[i] = c ? c : 'y';
        dst[i] = (char)__VERIFIER_nondet_int();
    }
    if (k < 0 || k >= SZ || size < 0 || size > SZ) return;
    src[k] = 0;
    int r = strscpy(dst, src, size);
    if (size == 0) {
        assert(r == -E2BIG);
        return;
    }
    if (k < size) {
        assert(r == k);
        if (0 <= j && j <= k) assert(dst[j] == src[j]);
    } else {
        assert(r == -E2BIG && dst[size - 1] == 0);
        if (0 <= j && j < size - 1) assert(dst[j] == src[j]);
    }
}

void bstrlcpy_contract(int k, int size, int j) {
    char src[SZ], dst[SZ];
    for (int i = 0; i < SZ; i++) {
        char c = (char)__VERIFIER_nondet_int();
        src[i] = c ? c : 'y';
    }
    if (k < 0 || k >= SZ || size < 1 || size > SZ) return;
    src[k] = 0;
    int r = bstrlcpy(dst, src, size, SZ);
    assert(r == k);
    int n = k < size - 1 ? k : size - 1;
    assert(dst[n] == 0);
    if (0 <= j && j < n) assert(dst[j] == src[j]);
}

/* Planted twin: strncpy-style copy that does not terminate on truncation. */
int strncpy_unterminated_bad(int k) {
    char src[SZ], dst[SZ];
    for (int i = 0; i < SZ; i++) src[i] = 'z';
    if (k < 0 || k >= SZ) return 0;
    src[k] = 0;
    for (int i = 0; i < SZ - 1 && src[i]; i++) dst[i] = src[i];
    int n = 0;
    while (dst[n]) n++; /* dst may be unterminated (and uninitialised) */
    return n;
}
