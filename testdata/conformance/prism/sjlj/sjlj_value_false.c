/* PRISM conformance task sjlj/sjlj_value_false.c: expected false (no-div0) */
#include <setjmp.h>
static void fail(jmp_buf* b, int code) { longjmp(*b, code); }
int sjlj_value_false(int x) {
    jmp_buf b;
    int r = setjmp(b);
    if (r == 0) {
        if (x > 5) fail(&b, x);
        return 0;
    }
    return 1000 / (r - 9); /* x == 9 */
}
