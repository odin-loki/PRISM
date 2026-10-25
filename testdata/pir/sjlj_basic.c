/* PIR tasks: setjmp/longjmp as exception-like edges (docs/PIR.md
 * "setjmp/longjmp"). setjmp returns 0 on the direct path and the longjmp
 * value (1 for 0) on the jump back; a longjmp to a jmp_buf whose setjmp
 * caller has returned is undefined behaviour (C11 7.13.2.1p2). */
#include <assert.h>
#include <setjmp.h>

static void fail(jmp_buf* b, int code) { longjmp(*b, code); }

int sjlj_ok(int x) {
    jmp_buf b;
    int r = setjmp(b);
    if (r == 0) {
        if (x > 5) fail(&b, 7);
        return 0;
    }
    assert(r == 7);
    return r;
}

int sjlj_zero_ok(int x) {
    jmp_buf b;
    int r = setjmp(b);
    if (r == 0) {
        if (x) longjmp(b, 0); /* longjmp(b, 0) makes setjmp return 1 */
        return 0;
    }
    assert(r == 1);
    return r;
}

int sjlj_bad(int x) {
    jmp_buf b;
    int r = setjmp(b);
    if (r == 0) {
        if (x > 5) fail(&b, x);
        return 0;
    }
    return 1000 / (r - 9); /* x == 9: division by zero after the jump back */
}

static int arm(jmp_buf* b) { return setjmp(*b); }

int sjlj_dead_frame_bad(int x) {
    jmp_buf b;
    if (arm(&b) != 0) return 1; /* setjmp's caller (arm) has returned ... */
    if (x) longjmp(b, 1);       /* ... so this longjmp is undefined behaviour */
    return 0;
}
