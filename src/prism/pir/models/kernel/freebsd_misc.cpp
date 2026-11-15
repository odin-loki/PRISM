/* FreeBSD user access, bus_space, delay, logging and panic specs (docs/KERNEL_MODELS.md).
 *
 * Reference semantics (sys/sys/systm.h, sys/sys/bus.h, sys/x86/include/bus.h,
 * copy(9), bus_space(9), printf(9), panic(9), DELAY(9); FreeBSD 15):
 *   copyin(udaddr, kaddr, len): returns 0 or EFAULT (14); the kernel buffer kaddr
 *     (len bytes, must be valid and writable) is then arbitrary (a failed copy may
 *     leave partial data). copyout(kaddr, udaddr, len): reads the kernel buffer
 *     (len bytes, must be valid), returns 0 or EFAULT. copyinstr(udaddr, kaddr,
 *     len, done): 0, EFAULT (14) or ENAMETOOLONG (63); kaddr arbitrary; *done (when
 *     non-NULL) is an arbitrary count in [0, len].
 *   bus_space_read_1/2/4/8(tag, handle, offset): an arbitrary value of that width;
 *     bus_space_write_*: no effect on memory.
 *   DELAY, pause_sbt, wakeup: no effect on memory (pause_sbt returns an arbitrary int).
 *   printf, device_printf, if_printf: no effect on memory, arguments evaluated.
 *   panic(fmt, ...) does not return: reaching it is a failure of the path.
 * Assumptions introduced (named in the verdict): user memory is untrusted
 * (arbitrary bytes, a copy may fail), the register space is the environment
 * (arbitrary reads, writes without effect on program memory, address and tag not
 * checked, no device model), delays and logging have no effect on memory.
 * Not specified here (needs review): the inline x86 bus_space_read_* (they cast
 * the integer handle to a pointer: not encodable), bus_dma, device_get_softc,
 * if_inc_counter, callout_*, taskqueue_*, sysctl_*, sbuf_*.
 */
#include "kernel_model.h"

extern "C" {

#define UA_NOTE "freebsd-copy: user memory is untrusted: bytes are arbitrary and a copy may fail with EFAULT (14); the user address is not checked"
#define BUS_NOTE "freebsd-bus-space: the register space is the environment: reads return an arbitrary value of their width, writes have no effect on program memory; tag, handle and offset are not checked; no device model"
#define LOGD_NOTE "freebsd-log: logging, delays and wakeups have no effect on memory the function can see; format arguments are not checked"

/* int copyin(const void *udaddr, void *kaddr, size_t len) */
int copyin(const void *udaddr, void *kaddr, size_t len) {
    (void)udaddr;
    ASSUME(UA_NOTE);
    __prism_havoc_bytes(kaddr, len);
    return CHOICE() ? 0 : 14;
}
/* int copyout(const void *kaddr, void *udaddr, size_t len) */
int copyout(const void *kaddr, void *udaddr, size_t len) {
    (void)udaddr;
    ASSUME(UA_NOTE);
    __prism_read_range(kaddr, len);
    return CHOICE() ? 0 : 14;
}
/* int copyinstr(const void *udaddr, void *kaddr, size_t len, size_t *lencopied) */
int copyinstr(const void *udaddr, void *kaddr, size_t len, size_t *lencopied) {
    (void)udaddr;
    ASSUME(UA_NOTE);
    __prism_havoc_bytes(kaddr, len);
    if (lencopied) {
        size_t n = __VERIFIER_nondet_ulong();
        __prism_assume(n <= len);
        *lencopied = n;
    }
    int r = __VERIFIER_nondet_int();
    __prism_assume(r == 0 || r == 14 || r == 63);
    return r;
}

/* bus_space(9): tag is a uintptr_t, handle a uintptr_t, offset a bus_size_t */
unsigned char bus_space_read_1(unsigned long tag, unsigned long handle, unsigned long off) {
    (void)tag; (void)handle; (void)off; ASSUME(BUS_NOTE); return __VERIFIER_nondet_uchar();
}
unsigned short bus_space_read_2(unsigned long tag, unsigned long handle, unsigned long off) {
    (void)tag; (void)handle; (void)off; ASSUME(BUS_NOTE); return __VERIFIER_nondet_ushort();
}
unsigned int bus_space_read_4(unsigned long tag, unsigned long handle, unsigned long off) {
    (void)tag; (void)handle; (void)off; ASSUME(BUS_NOTE); return __VERIFIER_nondet_uint();
}
unsigned long bus_space_read_8(unsigned long tag, unsigned long handle, unsigned long off) {
    (void)tag; (void)handle; (void)off; ASSUME(BUS_NOTE); return __VERIFIER_nondet_ulong();
}
void bus_space_write_1(unsigned long tag, unsigned long handle, unsigned long off, unsigned char v) {
    (void)tag; (void)handle; (void)off; (void)v; ASSUME(BUS_NOTE);
}
void bus_space_write_2(unsigned long tag, unsigned long handle, unsigned long off, unsigned short v) {
    (void)tag; (void)handle; (void)off; (void)v; ASSUME(BUS_NOTE);
}
void bus_space_write_4(unsigned long tag, unsigned long handle, unsigned long off, unsigned int v) {
    (void)tag; (void)handle; (void)off; (void)v; ASSUME(BUS_NOTE);
}
void bus_space_write_8(unsigned long tag, unsigned long handle, unsigned long off, unsigned long v) {
    (void)tag; (void)handle; (void)off; (void)v; ASSUME(BUS_NOTE);
}

/* void DELAY(int usec); int pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags); void wakeup(const void *chan) */
void DELAY(int usec) { (void)usec; ASSUME(LOGD_NOTE); }
int pause_sbt(const char *wmesg, long long sbt, long long pr, int flags) {
    (void)wmesg; (void)sbt; (void)pr; (void)flags;
    ASSUME(LOGD_NOTE);
    return __VERIFIER_nondet_int();
}
void wakeup(const void *chan) { (void)chan; ASSUME(LOGD_NOTE); }

int printf(const char *fmt, ...) { (void)fmt; ASSUME(LOGD_NOTE); return __VERIFIER_nondet_int(); }
int device_printf(void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOGD_NOTE); return __VERIFIER_nondet_int(); }
int if_printf(void *ifp, const char *fmt, ...) { (void)ifp; (void)fmt; ASSUME(LOGD_NOTE); return __VERIFIER_nondet_int(); }

/* void panic(const char *fmt, ...) __dead2 */
void panic(const char *fmt, ...) {
    (void)fmt;
    ASSUME("freebsd-panic: panic() does not return: reaching it is reported as FUNC-CONTRACT");
    __prism_check(0, "FUNC-CONTRACT", "panic() is reachable (the kernel does not return from it)");
    __builtin_trap();
}

}  // extern "C"
