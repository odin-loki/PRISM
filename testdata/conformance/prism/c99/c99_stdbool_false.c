// PRISM conformance task c99/c99_stdbool_false.c: expected false (no-div0)
#include <stdbool.h>
int c99_stdbool_false(int x) {
    bool z = (x != 0);
    return z ? 0 : 100 / x;
}
