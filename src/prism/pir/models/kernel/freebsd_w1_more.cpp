/* FreeBSD user byte fetch / compare-and-store, sx inline entry points, fillw and CRC32C specs
 * (docs/KERNEL_MODELS.md), W1 more.
 *
 * Reference semantics (FreeBSD 15: sys/sys/systm.h, fetch(9), casuword(9); sys/sys/sx.h;
 * sys/x86/include/x86_var.h, amd64/amd64/support.S; sys/sys/gsb_crc32.h):
 *   int fubyte(volatile const void *base): the byte at the user address, zero-extended (0..255),
 *     or -1 on a fault (the byte is zero-extended, so -1 is only a fault).
 *   int casueword32(volatile uint32_t *base, uint32_t oldval, uint32_t *oldvalp, uint32_t newval):
 *     0 on success (newval stored, the old value in *oldvalp), -1 on a fault (nothing stored),
 *     1 when the comparison or the store failed (nothing stored, *oldvalp holds the value read).
 *     The model leaves *oldvalp arbitrary on every outcome (a superset).
 *   int __sx_xlock(sx, td, opts, file, line): 0, or an error when SX_INTERRUPTIBLE is in opts;
 *     arbitrary int. void __sx_xunlock(sx, td, file, line): releases. Both read the lock word.
 *   void fillw(int pat, void *base, size_t cnt): stores the low 16 bits of pat into cnt
 *     consecutive 16-bit words at base (rep stosw). The model writes 2*cnt arbitrary bytes.
 *   uint32_t calculate_crc32c(uint32_t crc, const unsigned char *buf, unsigned int length): the
 *     CRC32C of length bytes of buf continuing from crc; the model returns an arbitrary value.
 * requires: sx points to a valid struct sx (8-byte lock word, as the existing sx specs); buf is
 *   readable for length bytes; base of fillw is writable for 2*cnt bytes.
 * Assumptions introduced (named in the verdict):
 *   - user mapping state is not tracked: fubyte / casueword32 succeed or fail arbitrarily
 *   - fubyte's byte value, calculate_crc32c's value and the bytes fillw writes are arbitrary
 *   - the lock state of __sx_xlock / __sx_xunlock is not tracked (as freebsd-lock)
 */
#include "kernel_model.h"

extern "C" {

/* int fubyte(volatile const void *base) (sys/sys/systm.h, fetch(9)) */
int fubyte(volatile const void *base) {
    ASSUME("freebsd-w1-fubyte: returns the user byte zero-extended (0..255) or -1 on a fault (fetch(9), FreeBSD 15); the user mapping is not tracked, so the outcome is arbitrary and the byte value is arbitrary");
    (void)base;
    if (CHOICE()) return -1;
    return __VERIFIER_nondet_uchar();
}

/* int casueword32(volatile uint32_t *base, uint32_t oldval, uint32_t *oldvalp, uint32_t newval) (sys/sys/systm.h, casuword(9)) */
int casueword32(volatile unsigned int *base, unsigned int oldval, unsigned int *oldvalp, unsigned int newval) {
    ASSUME("freebsd-w1-casueword32: 0 stores newval at the user address, -1 is a fault and 1 a failed comparison or store (nothing stored in both) (casuword(9), FreeBSD 15); the user mapping is not tracked, so the outcome is arbitrary; *oldvalp is arbitrary on every outcome");
    (void)oldval;
    int r = __VERIFIER_nondet_int();
    __prism_assume(r == 0 || r == 1 || r == -1);
    __prism_havoc_bytes(oldvalp, sizeof(unsigned int));
    if (r == 0) __prism_memcpy((void *)base, &newval, 4, 0);
    return r;
}

/* sys/sys/sx.h: static __inline int __sx_xlock(struct sx *sx, struct thread *td, int opts, const char *file, int line) */
int __sx_xlock(void *sx, void *td, int opts, const char *file, int line) {
    ASSUME("freebsd-w1-sx_xlock_inline: __sx_xlock returns 0, or an error when the lock is interruptible (arbitrary int); the lock state is not tracked and no data memory changes (sx.h, FreeBSD 15); the lock word must be valid memory");
    (void)td; (void)opts; (void)file; (void)line;
    __prism_read_range(sx, 8);
    return __VERIFIER_nondet_int();
}

/* sys/sys/sx.h: static __inline void __sx_xunlock(struct sx *sx, struct thread *td, const char *file, int line) */
void __sx_xunlock(void *sx, void *td, const char *file, int line) {
    ASSUME("freebsd-w1-sx_xunlock_inline: __sx_xunlock releases the exclusive lock; the lock state is not tracked and no data memory changes (sx.h, FreeBSD 15); the lock word must be valid memory");
    (void)td; (void)file; (void)line;
    __prism_read_range(sx, 8);
}

/* sys/x86/include/x86_var.h: void fillw(int pat, void *base, size_t cnt) */
void fillw(int pat, void *base, size_t cnt) {
    ASSUME("freebsd-w1-fillw: stores the low 16 bits of pat into cnt 16-bit words at base (amd64/amd64/support.S, FreeBSD 15); the model writes 2*cnt arbitrary bytes (a superset of the stored pattern); base must be writable for 2*cnt bytes");
    (void)pat;
    size_t n = cnt > PRISM_MAX_OBJ ? PRISM_MAX_OBJ + 1 : cnt * 2;
    __prism_havoc_bytes(base, n);
}

/* sys/sys/gsb_crc32.h: uint32_t calculate_crc32c(uint32_t crc32c, const unsigned char *buffer, unsigned int length) */
unsigned int calculate_crc32c(unsigned int crc32c, const unsigned char *buffer, unsigned int length) {
    ASSUME("freebsd-w1-calculate_crc32c: returns the CRC32C of length bytes of buffer continuing from crc32c (gsb_crc32.h, FreeBSD 15); the model returns an arbitrary value; buffer must be readable for length bytes");
    (void)crc32c;
    __prism_read_range(buffer, length);
    return __VERIFIER_nondet_uint();
}

}  // extern "C"
