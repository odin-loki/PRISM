/* PIR tasks: FreeBSD kernel API specs, W2 group a (docs/KERNEL_MODELS.md).
 * Run with `--kernel-api freebsd`. Prototypes as in the kernel headers. */

unsigned long strlcpy(char *dst, const char *src, unsigned long len);
unsigned long strlcat(char *dst, const char *src, unsigned long dstsize);

int strlcpy_ok(void) {
    char d[8];
    unsigned long r = strlcpy(d, "hello", sizeof d);
    return (r == 5 && d[0] == 'h' && d[5] == '\0') ? 0 : -1;
}
int strlcpy_bad(void) { /* dsize claims 8 but dst has 4 bytes: out-of-bounds write */
    char d[4];
    strlcpy(d, "hello", 8);
    return 0;
}

int strlcat_ok(void) {
    char d[8] = { 'a', 'b', '\0' };
    unsigned long r = strlcat(d, "cd", sizeof d);
    return (r == 4 && d[2] == 'c' && d[4] == '\0') ? 0 : -1;
}
int strlcat_bad(void) { /* siz claims 8 but dst has 4 bytes: out-of-bounds write */
    char d[4] = { 'a', 'b', '\0', '\0' };
    strlcat(d, "cdefgh", 8);
    return 0;
}
