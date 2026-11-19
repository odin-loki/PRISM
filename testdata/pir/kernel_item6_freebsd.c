/* PIR tasks: r1 FALSE triage item 6 (FreeBSD): sx locks as OpenZFS's mutex_enter / mutex_exit / mutex_tryenter
 * (spl/sys/mutex.h: sx_xlock, sx_xunlock, sx_try_xlock). Run with `--kernel-api freebsd`. `*_ok` functions are
 * PROVED-ASSUMING (freebsd-lock), `*_bad` functions are FAILED (tests/cpp/test_pir_triage_item6.cpp). */

typedef unsigned long uintptr_t;
struct sx { char lo[24]; volatile uintptr_t sx_lock; };
int _sx_xlock(struct sx *sx, int opts, const char *file, int line);
void _sx_xunlock(struct sx *sx, const char *file, int line);
int sx_try_xlock_int(struct sx *sx);
#define SX_INTERRUPTIBLE 0x40

/* mutex_enter(m); ...; mutex_exit(m): sx_xlock passes opts 0, so the lock is always taken */
int zfs_mutex_ok(void) {
    struct sx m = {{0}, 0};
    _sx_xlock(&m, 0, "f.c", 1);
    _sx_xunlock(&m, "f.c", 2);
    return 0;
}
/* an interruptible sleep can fail: releasing without checking the result can release a lock not held */
int sx_sig_bad(void) {
    struct sx m = {{0}, 0};
    (void)_sx_xlock(&m, SX_INTERRUPTIBLE, "f.c", 1);
    _sx_xunlock(&m, "f.c", 2);
    return 0;
}
/* mutex_tryenter: released only when taken */
int sx_try_ok(void) {
    struct sx m = {{0}, 0};
    if (sx_try_xlock_int(&m)) _sx_xunlock(&m, "f.c", 2);
    return 0;
}
/* the try can fail */
int sx_try_bad(void) {
    struct sx m = {{0}, 0};
    (void)sx_try_xlock_int(&m);
    _sx_xunlock(&m, "f.c", 2);
    return 0;
}
/* both ends visible, one path leaks */
int zfs_leak_bad(int c) {
    struct sx m = {{0}, 0};
    _sx_xlock(&m, 0, "f.c", 1);
    if (c) return 1;
    _sx_xunlock(&m, "f.c", 2);
    return 0;
}
