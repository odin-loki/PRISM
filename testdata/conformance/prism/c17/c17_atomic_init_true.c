// PRISM conformance task c17/c17_atomic_init_true.c: expected true (no-overflow)
#include <stdatomic.h>
int c17_atomic_init_true(int x) {
    atomic_int c;
    atomic_init(&c, x & 0xFF);
    return atomic_load(&c) * 1000;
}
