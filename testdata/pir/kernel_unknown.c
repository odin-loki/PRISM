/* PIR tasks: calls without a spec (docs/KERNEL_MODELS.md "Policy"). By default
 * every call to a function the unit does not define and no model covers is
 * `UNENCODED: call @f` (NEEDS-HARNESS); `--assume-unknown-calls` is the opt-in:
 * the result and the objects the pointer arguments point into are havocked, the
 * callee is assumed to return and to change nothing else, and the call is named
 * in the assumption list (`unknown-call: @f ...`), so a proof is PROVED-ASSUMING. */

int e1000e_poll_hw(int reg);                     /* driver-local: no spec */
void e1000e_fill(char *buf, unsigned long n);     /* writes through its pointer argument */
char *e1000e_get_buf(int which);                  /* returns NULL or a pointer */
int e1000e_name(const char *s);                   /* takes a string */

int unk_result_ok(void) {
    int r = e1000e_poll_hw(3);
    return 100 / ((r & 0xff) | 1); /* any result */
}
int unk_result_bad(void) { /* the result can be 0 */
    int r = e1000e_poll_hw(3);
    return 100 / r;
}
int unk_nopointer_keeps_locals_ok(void) { /* a local whose address is not passed is unchanged */
    int x = 5;
    e1000e_poll_hw(x);
    return 100 / (x - 4);
}
int unk_pointer_arg_ok(void) { /* the buffer passed is written: its bytes are initialised and arbitrary */
    char buf[8];
    e1000e_fill(buf, 8);
    return buf[7] & 1;
}
int unk_pointer_arg_bad(void) { /* ... so a value read back can be 0 */
    char buf[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    e1000e_fill(buf, 8);
    return 100 / buf[0];
}
int unk_literal_arg_ok(void) { /* a string literal is read-only: passing it is not a write */
    return e1000e_name("literal") & 0;
}
int unk_ptr_result_ok(void) {
    char *p = e1000e_get_buf(1);
    if (!p) return -12;
    return p[10] & 1;
}
int unk_ptr_result_nocheck_bad(void) { /* NULL is possible */
    char *p = e1000e_get_buf(1);
    return p[10];
}
