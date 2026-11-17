/* FreeBSD string function specs (docs/KERNEL_MODELS.md), W2 group a.
 *
 * Reference semantics (FreeBSD 15 kernel source):
 *   strlcpy(dst, src, dsize)  (sys/libkern/strlcpy.c:27-50): copies at most dsize-1 bytes of
 *     src and NUL-terminates dst (when dsize != 0); always reads src up to its NUL;
 *     returns strlen(src). With dsize == 0 nothing is written.
 *   strlcat(dst, src, siz)  (sys/libkern/strlcat.c:42-68): finds the NUL of dst within the
 *     first siz bytes, appends src truncated so that the result fits in siz bytes and is
 *     NUL-terminated; returns dlen + strlen(src) (dlen = siz if dst has no NUL in siz bytes,
 *     in which case dst is not written).
 * Failure paths: none (these functions do not fail; truncation is reported by the result).
 * The models follow the reference code byte for byte, so each reads and writes exactly the
 * bytes the real function does; a violated precondition is an out-of-bounds access found by
 * the memory model.
 * Not specified: strncmp (the libc model of the same name is selected in kernel mode, see NEEDS_REVIEW).
 * requires: strlcpy: src is a NUL-terminated string; dst is writable for min(strlen(src)+1,
 *   dsize) bytes.
 * strlcat: dst is readable/writable within siz bytes as above and src is
 *   NUL-terminated.
 * Assumptions introduced (named in the verdict):
 *   - the models reproduce the reference source; overlapping dst and src (restrict) are not
 *     modelled
 */
#include "kernel_model.h"

extern "C" {

/* size_t strlcpy(char *dst, const char *src, size_t len) (sys/libkern/strlcpy.c:27-50) */
unsigned long strlcpy(char *dst, const char *src, unsigned long dsize) {
    ASSUME("freebsd-w2-strlcpy: copies up to dsize-1 bytes of src, NUL-terminates dst when dsize != 0, returns strlen(src) (sys/libkern/strlcpy.c:27-50); overlap of dst and src is not modelled");
    unsigned long n = 0;
    while (src[n] != '\0') n++;
    if (dsize != 0) {
        unsigned long c = (n < dsize - 1) ? n : dsize - 1;
        for (unsigned long i = 0; i < c; i++) dst[i] = src[i];
        dst[c] = '\0';
    }
    return n;
}

/* size_t strlcat(char *dst, const char *src, size_t dstsize) (sys/libkern/strlcat.c:42-68) */
unsigned long strlcat(char *dst, const char *src, unsigned long siz) {
    ASSUME("freebsd-w2-strlcat: appends src to the string in dst within siz bytes, truncating, NUL-terminated, returns dlen + strlen(src) (sys/libkern/strlcat.c:42-68)");
    char *d = dst;
    const char *s = src;
    unsigned long n = siz;
    unsigned long dlen;
    while (n-- != 0 && *d != '\0') d++;
    dlen = (unsigned long)(d - dst);
    n = siz - dlen;
    if (n == 0) {
        unsigned long l = 0;
        while (s[l] != '\0') l++;
        return dlen + l;
    }
    while (*s != '\0') {
        if (n != 1) {
            *d++ = *s;
            n--;
        }
        s++;
    }
    *d = '\0';
    return dlen + (unsigned long)(s - src);
}

}  // extern "C"
