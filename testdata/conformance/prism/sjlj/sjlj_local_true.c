/* PRISM conformance task sjlj/sjlj_local_true.c: expected true (no-div0) */
#include <setjmp.h>
int sjlj_local_true(int x) {
    jmp_buf b;
    int d = 1, e = 0;
    if (setjmp(b)) return 100 / d; /* d is not modified after setjmp */
    e = 5;
    if (x) longjmp(b, e);
    return 0;
}
