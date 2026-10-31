/* Minimal <stdatomic.h> for PIR LP64 lowering on Windows (tests/conc). */
/*
 * Windows PIR compile shim. The MSVC C runtime has no usable C11
 * <stdatomic.h> for this lowering, so the operations map straight onto
 * clang's __c11_atomic builtins (PIR always compiles with clang). The IR
 * therefore carries the real memory order, exactly as with glibc:
 * seq_cst stays SC, and relaxed/acquire/release reach the conc stage as
 * monotonic/acquire/release, which it refuses as NEEDS-HARNESS. This header
 * is a declaration shim, not a proof.
 */
#ifndef PRISM_PIR_SHIM_STDATOMIC_H
#define PRISM_PIR_SHIM_STDATOMIC_H

typedef enum {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

typedef _Atomic(int) atomic_int;

#define atomic_store_explicit(obj, desired, order) __c11_atomic_store((obj), (desired), (order))
#define atomic_load_explicit(obj, order) __c11_atomic_load((obj), (order))
#define atomic_fetch_add_explicit(obj, arg, order) __c11_atomic_fetch_add((obj), (arg), (order))
#define atomic_store(obj, desired) atomic_store_explicit((obj), (desired), memory_order_seq_cst)
#define atomic_load(obj) atomic_load_explicit((obj), memory_order_seq_cst)
#define atomic_fetch_add(obj, arg) atomic_fetch_add_explicit((obj), (arg), memory_order_seq_cst)

#endif
