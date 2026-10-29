// PIR task: an SV-COMP unit (it has the reach_error sink). The sink's own body
// is not a defect (reaching it is checked at each call site), and abort() is
// the benchmark's assume(false), as in the bmc encoder.
extern void abort(void);
extern void __assert_fail(const char *, const char *, unsigned int, const char *) __attribute__((__noreturn__));
void reach_error(void) { __assert_fail("0", "svcomp_sink.c", 6, "reach_error"); }
extern unsigned int __VERIFIER_nondet_uint(void);
int sink_assume_ok(void) {
    unsigned int x = __VERIFIER_nondet_uint();
    if (x < 268435455u) abort();
    if (!(x >= 0x0fffffffu)) reach_error();
    return 0;
}
int sink_reach_bad(void) {
    unsigned int x = __VERIFIER_nondet_uint();
    if (x > 10u) reach_error();
    return 0;
}
