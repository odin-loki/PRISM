// PRISM conformance task c11/c11_noreturn_false.c: expected false (no-div0)
#include <stdlib.h>
_Noreturn static void die(void) { abort(); }
int c11_noreturn_false(int x) {
    if (x == 1) die();
    return 100 / x;
}
