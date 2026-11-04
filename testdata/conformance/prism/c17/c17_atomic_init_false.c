// PRISM conformance task c17/c17_atomic_init_false.c: expected false (no-overflow)
#include <stdatomic.h>
int c17_atomic_init_false(int x) {
    atomic_int c;
    atomic_init(&c, x);
    return atomic_load(&c) * 1000;
}
