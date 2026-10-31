// PRISM conformance task c11/c11_atomic_true.c: expected true (no-overflow)
#include <stdatomic.h>
int c11_atomic_true(int x) {
    atomic_int c = 0;
    atomic_fetch_add(&c, x & 0xFF);
    return atomic_load(&c) * 1000;
}
