/* Kernel API specs (docs/KERNEL_MODELS.md). Same mechanism as the libc models
 * (models/libc): C++ sources with extern "C" kernel API, lowered by clang,
 * linked into a unit for each function it declares but does not define, and only
 * with `--kernel-api linux|freebsd` (files linux_*.cpp, freebsd_*.cpp).
 *
 * A spec states, in its comment, (1) the contract on a reference semantics,
 * (2) the failure paths, (3) the assumptions it introduces (each one a
 * string passed to ASSUME: it appears in the verdict's assumption list and a
 * proof is PROVED-ASSUMING), (4) its source (kernel header or manual page). A
 * spec must never be stronger than the real function: where the real
 * behaviour is unknown the model returns an arbitrary value.
 * Preconditions are checked by the memory model (a `requires:` comment names
 * them), exactly like the libc models.
 */
#ifndef PRISM_KERNEL_MODEL_H
#define PRISM_KERNEL_MODEL_H
#include "prism_model.h"

#define ASSUME(text) __prism_assumption(text)
/* an arbitrary choice made by the environment (failure or success, which of two results) */
#define CHOICE() (__VERIFIER_nondet_int() != 0)

#endif
