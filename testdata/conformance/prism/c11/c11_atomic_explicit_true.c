// PRISM conformance task c11/c11_atomic_explicit_true.c: expected true (no-overflow)
#include <stdatomic.h>
int c11_atomic_explicit_true(short x) {
    atomic_int c = 0;
    atomic_fetch_add_explicit(&c, x, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    return atomic_load_explicit(&c, memory_order_acquire) + 1;
}
