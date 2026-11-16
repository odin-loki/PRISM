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
    long v = 0;
    if (fueword((volatile const void *)0x1000, &v)) return -1; /* EFAULT */
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
