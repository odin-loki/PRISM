/* PIR tasks: FreeBSD kernel API specs, W1 groups f2 and f3 (docs/KERNEL_MODELS.md).
 * Run with `--kernel-api freebsd`. fueword(9) fetches a word from user space and
 * returns 0 on success, -1 on fault (*val written only on success). suword(9) and
 * subyte(9) store at a user address and return 0 or -1; both are __nodiscard, so the
 * result must be checked before the stored value is relied on. The declarations are
 * the kernel's (sys/sys/systm.h). */

int fueword(volatile const void *base, long *val);
int suword(volatile void *base, long word);
int subyte(volatile void *base, int byte);
int suword32(volatile void *base, int word);
int fueword32(volatile const void *base, int *val);
void critical_enter(void);
void critical_exit(void);

int suword32_ok(void) {
    volatile int buf[2] = {0, 0};
    if (suword32((volatile void *)&buf[0], 0x41424344)) return -1;
    return buf[0] == 0x41424344 ? 0 : 1; /* stored on the success path */
}
int suword32_bad(void) {
    volatile int buf[1] = {0};
    suword32((volatile void *)buf, 1); /* __nodiscard-style: the fault is not checked */
    return 100 / buf[0];               /* fault path: buf[0] is still 0 */
}
int fueword32_ok(void) {
    volatile int ubuf[2] = {1, 2};
    int v;
    if (fueword32((volatile const void *)ubuf, &v)) return -1;
    return v & 1; /* any loaded value is acceptable on success */
}
int fueword32_bad(void) { /* unchecked result: on the fault path v stays 0 */
    volatile int ubuf[2] = {1, 2};
    int v = 0;
    (void)fueword32((volatile const void *)ubuf, &v);
    return 100 / v;
}
int critical_enter_ok(void) {
    critical_enter();
    critical_exit();
    return 0;
}
int critical_exit_ok(void) {
    critical_enter();
    critical_exit();
    return 0;
}

int fueword_ok(void) { /* result checked before *val is used */
    volatile long ubuf[1] = {7}; /* stands for a user word: a constant address is not encodable */
    long v = 0;
    if (fueword((volatile const void *)ubuf, &v)) return -1; /* EFAULT */
    return (int)(v & 1); /* v is arbitrary on success: any use is allowed */
}

int suword_ok(void) {
    long buf[2];
    if (suword(buf, 42) == 0)
        return (int)(buf[0] - 42); /* stored on the success path */
    return 0;
}
int suword_bad(void) {
    long buf[1] = {0};
    suword(buf, 7);            /* __nodiscard: failure not checked */
    return 100 / (int)buf[0];  /* failure path: buf[0] is still 0 */
}

int subyte_ok(void) {
    char buf[2];
    if (subyte(buf, 7) == 0)
        return buf[0] - 7;     /* stored on the success path */
    return 0;
}
int subyte_bad(void) {
    char buf[1] = {0};
    subyte(buf, 7);            /* __nodiscard: failure not checked */
    return 100 / buf[0];       /* failure path: buf[0] is still 0 */
}

/* ---- W1 more: fubyte, casueword32, __sx_xlock / __sx_xunlock, fillw, calculate_crc32c ---- */
int fubyte(volatile const void *base);
int casueword32(volatile unsigned int *base, unsigned int oldval, unsigned int *oldvalp, unsigned int newval);
int __sx_xlock(void *sx, void *td, int opts, const char *file, int line);
void __sx_xunlock(void *sx, void *td, const char *file, int line);
void fillw(int pat, void *base, unsigned long cnt);
unsigned int calculate_crc32c(unsigned int crc32c, const unsigned char *buffer, unsigned int length);

int fubyte_ok(void) { /* the byte is 0..255 or -1: never above 255 */
    volatile unsigned char u[1] = {7};
    int r = fubyte((volatile const void *)u);
    return r > 255 ? -1 : 0;
}
int fubyte_bad(void) { /* r + 1 is 0 on a fault */
    volatile unsigned char u[1] = {7};
    return 100 / (fubyte((volatile const void *)u) + 1);
}
int casueword32_ok(void) {
    volatile unsigned int u[1] = {5};
    unsigned int old = 0;
    int r = casueword32(u, 5, &old, 9);
    if (r == 0) return u[0] == 9 ? 0 : -1; /* stored on success only */
    return r == 1 || r == -1 ? 0 : -1;
}
int casueword32_bad(void) { /* unchecked result: on the failure paths the word is unchanged */
    volatile unsigned int u[1] = {5};
    unsigned int old = 0;
    (void)casueword32(u, 5, &old, 9);
    return 100 / ((int)u[0] - 5);
}
int __sx_xlock_ok(void) {
    long sx[4] = {0, 0, 0, 0}; /* stands for a struct sx */
    char td;
    int e = __sx_xlock(sx, &td, 0, "f.c", 1);
    (void)e; /* 0, or an error with SX_INTERRUPTIBLE */
    return 0;
}
int __sx_xlock_bad(void) { /* lock word smaller than 8 bytes */
    int sx[1] = {0};
    char td;
    return __sx_xlock(sx, &td, 0, "f.c", 1);
}
int __sx_xunlock_ok(void) {
    long sx[4] = {0, 0, 0, 0};
    char td;
    __sx_xunlock(sx, &td, "f.c", 1);
    return 0;
}
int __sx_xunlock_bad(void) {
    __sx_xunlock((void *)0, (void *)0, "f.c", 1);
    return 0;
}
int fillw_ok(void) {
    unsigned short b[4] = {0, 0, 0, 0};
    fillw(0x1234, b, 4); /* 8 bytes */
    return 0;
}
int fillw_bad(void) { /* 5 words into an 8-byte object */
    unsigned short b[4] = {0, 0, 0, 0};
    fillw(0x1234, b, 5);
    return 0;
}
int calculate_crc32c_ok(void) {
    unsigned char b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned int c = calculate_crc32c(0xffffffffu, b, 8);
    (void)c;
    return 0;
}
int calculate_crc32c_bad(void) { /* length 9 on an 8-byte buffer */
    unsigned char b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    return (int)calculate_crc32c(0xffffffffu, b, 9);
}
