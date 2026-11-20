/* Linux user-space access specs (docs/KERNEL_MODELS.md).
 *
 * Reference semantics (include/linux/uaccess.h, lib/usercopy.c,
 * arch/x86/include/asm/uaccess.h; Linux 6.18). Every function returns the number
 * of bytes NOT copied (0 = all copied); user memory is the environment:
 *   _copy_from_user(to, from, n): reads n bytes at the user address `from`; the
 *     kernel buffer `to` (n bytes, must be valid and writable) receives the copied
 *     bytes, arbitrary, and the rest zero; returns in [0, n].
 *   _copy_to_user(to, from, n): reads the kernel buffer `from` (n bytes, must be
 *     valid: a NULL or freed or too small source is reported); user memory is not
 *     modelled and nothing in the program's memory changes; returns in [0, n].
 *   strncpy_from_user(dst, src, count): -EFAULT (-14) or the string length in
 *     [0, count]; dst (count bytes, must be valid) receives arbitrary bytes.
 * The raw_copy_* variants have the same contract without the zero padding
 * (the model's bytes are arbitrary either way).
 * Assumptions introduced (named in the verdict):
 *   - user memory is untrusted: the bytes read from it are arbitrary, any copy
 *     may fail part way (return in [0, n])
 *   - the user address itself is not checked (access_ok is the caller's code);
 *     user and kernel address spaces are not modelled as objects
 * Note: with CONFIG_X86 the kernel's copy_from_user is an inline function over
 * inline assembly (`rep movsb`), which --kernel-api models as asm-havoc (a havoc
 * of the objects its pointer operands point into), not through these specs.
 */
#include "kernel_model.h"

extern "C" {

static unsigned long copy_in(void *to, unsigned long n) {
    ASSUME("linux-uaccess: user memory is untrusted: copied bytes are arbitrary, a copy may fail part way (returns the bytes not copied, in [0, n]); the user address is not checked");
    __prism_havoc_bytes(to, n);
    unsigned long left = __VERIFIER_nondet_ulong();
    __prism_assume(left <= n);
    return left;
}

static unsigned long copy_out(const void *from, unsigned long n) {
    ASSUME("linux-uaccess: user memory is untrusted: a copy may fail part way (returns the bytes not copied, in [0, n]); the user address is not checked and user memory is not modelled");
    __prism_read_range(from, n);
    unsigned long left = __VERIFIER_nondet_ulong();
    __prism_assume(left <= n);
    return left;
}

unsigned long _copy_from_user(void *to, const void *from, unsigned long n) { (void)from; return copy_in(to, n); }
unsigned long copy_from_user(void *to, const void *from, unsigned long n) { (void)from; return copy_in(to, n); }
unsigned long raw_copy_from_user(void *to, const void *from, unsigned long n) { (void)from; return copy_in(to, n); }
unsigned long _copy_to_user(void *to, const void *from, unsigned long n) { (void)to; return copy_out(from, n); }
unsigned long copy_to_user(void *to, const void *from, unsigned long n) { (void)to; return copy_out(from, n); }
unsigned long raw_copy_to_user(void *to, const void *from, unsigned long n) { (void)to; return copy_out(from, n); }

/* long strncpy_from_user(char *dst, const char __user *src, long count): -EFAULT or the length (< count when the
 * string ended, count when it did not); requires: dst is writable for count bytes (count >= 0) */
long strncpy_from_user(char *dst, const char *src, long count) {
    (void)src;
    ASSUME("linux-uaccess: user memory is untrusted: copied bytes are arbitrary, a copy may fail part way (returns the bytes not copied, in [0, n]); the user address is not checked");
    if (count < 0) __prism_check(0, "FUNC-CONTRACT", "strncpy_from_user with a negative count");
    long r = __VERIFIER_nondet_long();
    __prism_assume(r == -14 || (r >= 0 && r <= count));
    __prism_havoc_bytes(dst, static_cast<size_t>(count));
    return r;
}

}  // extern "C"
