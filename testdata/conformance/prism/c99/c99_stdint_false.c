// PRISM conformance task c99/c99_stdint_false.c: expected false (no-overflow)
#include <stdint.h>
int c99_stdint_false(int x) {
    int32_t a = (int32_t)x;
    return (int)(a * (int32_t)1000);
}
