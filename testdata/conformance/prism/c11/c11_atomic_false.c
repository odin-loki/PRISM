// PRISM conformance task c11/c11_atomic_false.c: expected false (no-overflow)
#include <stdatomic.h>
int c11_atomic_false(int x) {
    atomic_int c = 0;
    atomic_fetch_add(&c, x);
    return atomic_load(&c) * 1000;
}
