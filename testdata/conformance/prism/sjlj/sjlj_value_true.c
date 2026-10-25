/* PRISM conformance task sjlj/sjlj_value_true.c: expected true (no-div0) */
#include <setjmp.h>
static void fail(jmp_buf* b, int code) { longjmp(*b, code); }
int sjlj_value_true(int x) {
    jmp_buf b;
    int r = setjmp(b);
    if (r == 0) {
        if (x > 5) fail(&b, 7);
        return 0;
    }
    return 1000 / (r - 6);
}
