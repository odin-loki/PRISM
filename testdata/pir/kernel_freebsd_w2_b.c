/* PIR tasks: FreeBSD kernel API specs, W2 group b (docs/KERNEL_MODELS.md).
 * Run with `--kernel-api freebsd`. The declaration is the kernel's own
 * (sys/contrib/ncsw/inc/stdlib_ext.h). */

char *strncpy(char *dest, const char *src, unsigned long count);

int strncpy_ok(void) {
    char src[4] = { 'a', 'b', 'c', 0 };
    char dst[8];
    strncpy(dst, src, 8); /* copies "abc", pads dst[3..7] with NUL */
    return (dst[0] == 'a' && dst[2] == 'c' && dst[7] == 0) ? 0 : -1;
}

int strncpy_bad(void) { /* 8 bytes are copied into a 4-byte object: out-of-bounds write */
    char src[9] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0 };
    char dst[4];
    strncpy(dst, src, 8);
    return dst[0];
}
