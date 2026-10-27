/* Minimal <stdatomic.h> for PIR LP64 lowering on Windows (tests/conc). */
/*
 * Windows PIR compile shim. It lowers C11 atomics to plain stores.
 * memory_order is ignored. Relaxed, acquire, and release are not modeled.
 * It is not a concurrency proof and it is not sequentially consistent.
 * The conc stage is what refuses those orders as NEEDS-HARNESS. This
 * header must not be read as PROVED or as CLEAN. It is not a proof.
 */
#ifndef PRISM_PIR_SHIM_STDATOMIC_H
#define PRISM_PIR_SHIM_STDATOMIC_H

typedef enum { memory_order_relaxed, memory_order_consume, memory_order_acquire, memory_order_release,
               memory_order_acq_rel, memory_order_seq_cst } memory_order;

typedef int atomic_int;

static inline void atomic_store_explicit(volatile atomic_int* obj, int desired, memory_order order) {
    (void)order;
    *obj = desired;
}

static inline int atomic_load_explicit(const volatile atomic_int* obj, memory_order order) {
    (void)order;
    return *obj;
}

static inline int atomic_fetch_add(volatile atomic_int* obj, int arg) {
    int old = *obj;
    *obj = old + arg;
    return old;
}

#define atomic_load(obj) atomic_load_explicit(obj, memory_order_seq_cst)

#endif
