// PRISM conformance task c11/c11_noreturn_true.c: expected true (no-div0)
#include <stdlib.h>
_Noreturn static void die(void) { abort(); }
int c11_noreturn_true(int x) {
    if (x == 0) die();
    return 100 / x;
}
