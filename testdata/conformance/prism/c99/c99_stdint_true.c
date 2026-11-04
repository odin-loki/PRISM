// PRISM conformance task c99/c99_stdint_true.c: expected true (no-overflow)
#include <stdint.h>
int c99_stdint_true(int x) {
    int32_t a = (int32_t)(x & 0xFFFF);
    return (int)(a * (int32_t)1000);
}
