/* PIR tasks: FreeBSD kernel API specs (docs/KERNEL_MODELS.md). Run with
 * `--kernel-api freebsd`. FreeBSD's malloc(9) takes a type and flags: with M_WAITOK
 * it cannot return NULL (malloc(9)), with M_NOWAIT it can. The declarations are the
 * kernel's (sys/sys/malloc.h, mutex.h, systm.h). */

typedef unsigned long size_t;
struct malloc_type { long ks_magic; };
extern struct malloc_type M_TEST[1];
void *malloc(size_t size, struct malloc_type *type, int flags);
void free(void *addr, struct malloc_type *type);
#define M_NOWAIT 0x0001
#define M_WAITOK 0x0002
#define M_ZERO 0x0100

typedef unsigned long uintptr_t;
void __mtx_lock_flags(volatile uintptr_t *c, int opts, const char *file, int line);
void __mtx_unlock_flags(volatile uintptr_t *c, int opts, const char *file, int line);
struct mtx { char lo[24]; volatile uintptr_t mtx_lock; };

int copyin(const void *udaddr, void *kaddr, size_t len);
int copyout(const void *kaddr, void *udaddr, size_t len);
char user_buf[64]; /* stands for a user buffer */
int device_printf(void *dev, const char *fmt, ...);
void DELAY(int usec);
void panic(const char *fmt, ...);
unsigned int bus_space_read_4(unsigned long tag, unsigned long handle, unsigned long off);
void bus_space_write_4(unsigned long tag, unsigned long handle, unsigned long off, unsigned int v);

int waitok_ok(void) { /* M_WAITOK never returns NULL: no check needed */
    char *p = malloc(16, M_TEST, M_WAITOK);
    p[15] = 1;
    free(p, M_TEST);
    return 0;
}
int nowait_nocheck_bad(void) { /* M_NOWAIT can return NULL */
    char *p = malloc(16, M_TEST, M_NOWAIT);
    p[15] = 1;
    free(p, M_TEST);
    return 0;
}
int nowait_ok(void) {
    char *p = malloc(16, M_TEST, M_NOWAIT | M_ZERO);
    if (!p) return 12;
    int r = p[3];
    free(p, M_TEST);
    return 100 / (r + 1); /* M_ZERO: r == 0 */
}
int waitok_uninit_bad(void) {
    char *p = malloc(16, M_TEST, M_WAITOK);
    int r = p[3];
    free(p, M_TEST);
    return r;
}
int free_twice_bad(void) {
    char *p = malloc(16, M_TEST, M_WAITOK);
    free(p, M_TEST);
    free(p, M_TEST);
    return 0;
}
int mtx_ok(void) {
    struct mtx m = {{0}, 0};
    __mtx_lock_flags(&m.mtx_lock, 0, "f.c", 1);
    __mtx_unlock_flags(&m.mtx_lock, 0, "f.c", 2);
    return 0;
}
int mtx_null_bad(void) {
    __mtx_lock_flags((volatile uintptr_t *)0, 0, "f.c", 1);
    return 0;
}
int copyin_ok(void) {
    char kbuf[16];
    if (copyin(user_buf, kbuf, 16)) return 14; /* EFAULT */
    return kbuf[15] & 1;
}
int copyin_trust_bad(void) { /* user data is arbitrary */
    char kbuf[16];
    if (copyin(user_buf, kbuf, 16)) return 14;
    return 100 / kbuf[0];
}
int copyin_oob_bad(void) {
    char kbuf[8];
    return copyin(user_buf, kbuf, 16);
}
int copyout_oob_bad(void) {
    char kbuf[8] = {0};
    return copyout(kbuf, user_buf, 16);
}
int bus_ok(void) {
    bus_space_write_4(1, 0x1000, 0x10, 1);
    unsigned int v = bus_space_read_4(1, 0x1000, 0x10);
    return 100 / (int)((v & 0xff) | 1);
}
int bus_trust_bad(void) {
    unsigned int v = bus_space_read_4(1, 0x1000, 0x10);
    return 100 / (int)(v & 0xff);
}
int log_ok(void) {
    int x = 3;
    device_printf((void *)0, "x=%d %b", x, 5);
    DELAY(10);
    return 100 / x;
}
int panic_bad(int n) {
    if (n == 5) panic("bad %d", n);
    return n;
}
