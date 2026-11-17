/* FreeBSD string copy spec (docs/KERNEL_MODELS.md), W2 group b.
 *
 * Reference semantics (sys/libkern/strncpy.c:42-59; sys/contrib/ncsw/inc/stdlib_ext.h:113; FreeBSD 15):
 *   char *strncpy(char *dst, const char *src, size_t n): copies at most n bytes from src to dst,
 *     stopping after the first NUL byte copied; if the NUL is reached before n bytes, the rest
 *     of dst (up to n bytes) is padded with NUL bytes. Returns dst. When n == 0 nothing is
 *     read or written. The result is not NUL-terminated if src has no NUL in its first n bytes.
 * Failure paths: none; the function cannot fail.
 * requires: src is readable up to and including its first NUL byte, or n bytes if it has
 *   no NUL within them; dst is writable for n bytes (n == 0: nothing is accessed). The
 *   objects must not overlap (__restrict in the source).
 * Assumptions introduced (named in the verdict):
 *   - the model is the reference loop itself: it reads src byte by byte exactly as far as
 *     the real function does and writes exactly n bytes of dst
 *
 * Not specified (see NEEDS_REVIEW): if_getflags, if_getcapenable (no definition in the
 *   reference tree), sbuf_cat (writes struct sbuf internals, not modelled), m_pullup
 *   (frees and allocates mbufs, depends on mbuf layout).
 */
#include "kernel_model.h"

extern "C" {

/* char *strncpy(char *dest, const char *src, size_t count) (sys/libkern/strncpy.c) */
char *strncpy(char *dst, const char *src, unsigned long n) {
    ASSUME("freebsd-w2-strncpy: copies src to dst up to and including the first NUL, at most n bytes, then NUL-pads dst up to n bytes, returns dst (sys/libkern/strncpy.c:42-59, FreeBSD 15); the model is the reference loop, src is read only as far as the real function reads it");
    unsigned long i = 0;
    for (; i < n; i++) {
        char c = src[i];
        dst[i] = c;
        if (c == '\0') {
            for (i++; i < n; i++)
                dst[i] = '\0';
            break;
        }
    }
    return dst;
}

}  // extern "C"
