/* PRISM conformance task sjlj/sjlj_local_false.c: expected false (no-div0) */
#include <setjmp.h>
int sjlj_local_false(int x) {
    jmp_buf b;
    int d = 1;
    if (setjmp(b)) return 100 / d;
    d = 0; /* modified after setjmp: indeterminate after the jump (0 at -O0) */
    if (x) longjmp(b, 1);
    return 0;
}
