// PRISM conformance task c11/c11_atomic_explicit_false.c: expected false (no-overflow)
#include <stdatomic.h>
int c11_atomic_explicit_false(int x) {
    atomic_int c = 1;
    atomic_fetch_add_explicit(&c, 1, memory_order_relaxed);
    return atomic_load_explicit(&c, memory_order_acquire) + x;
}
