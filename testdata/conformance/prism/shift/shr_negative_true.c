// PRISM conformance task shift/shr_negative_true.c: expected true (no-shift-ub)
// right shift of a negative value is implementation-defined, not UB
int shr_negative_true(int a) {
    return a >> 3;
}
