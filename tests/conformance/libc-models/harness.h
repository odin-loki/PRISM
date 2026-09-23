/* Contract harnesses for PRISM's libc operational models (roadmap 8.2,
 * "libc models verified by PRISM"; docs/PIR.md "Library models").
 *
 * Every harness file #includes the model source it checks from
 * src/prism/pir/models/libc/, so PRISM's pir stage analyses the repository's
 * model code itself (inlined at the harness's call, like any function of the
 * unit), not the platform libc. A harness builds its own objects with
 * symbolic contents (`nd_char`) and symbolic sizes/positions (its scalar
 * parameters), calls the model, and states the C standard's contract with
 * assert(): a reachable failing assert is FUNC-CONTRACT, and every load and
 * store inside the model is checked by the memory model on the way.
 *
 * `_true` harnesses: the model meets the contract for every input.
 * `_false` harnesses: a wrong contract or a precondition violation; PRISM
 * must refute them (this keeps the `_true` proofs from being vacuous).
 */
#ifndef PRISM_LIBC_HARNESS_H
#define PRISM_LIBC_HARNESS_H

#include "../../../src/prism/pir/models/libc/prism_model.h"

void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function)
    __attribute__((__noreturn__));
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))

void __VERIFIER_assume(int cond);

/* Buffer size of the harnesses: loops in the models run at most N + 1 times,
 * below the default --unwind 8, so a verdict is PROVED only when the
 * unwinding assertion is closed as well (never folded from BOUNDED, Law 2). */
#define N 4

static inline char nd_char(void) { return (char)__VERIFIER_nondet_int(); }

/* s[0..cap-1] gets arbitrary non-NUL bytes and a NUL at k (k < cap). */
#define MAKE_STR(s, cap, k)                                   \
    do {                                                      \
        for (int i_ = 0; i_ < (cap); i_++) {                  \
            char c_ = nd_char();                              \
            __VERIFIER_assume(c_ != 0);                       \
            (s)[i_] = c_;                                     \
        }                                                     \
        (s)[(k)] = 0;                                         \
    } while (0)

/* b[0..cap-1] gets arbitrary bytes (NUL included). */
#define MAKE_BYTES(b, cap)                                    \
    do {                                                      \
        for (int i_ = 0; i_ < (cap); i_++) (b)[i_] = nd_char(); \
    } while (0)

#endif
