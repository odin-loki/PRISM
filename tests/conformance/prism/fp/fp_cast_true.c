/* PRISM conformance task fp/fp_cast_true.c: expected true (no-fp-cast) */
int fp_cast_true(int x) {
    double d = x * 1000.0;
    if (d > 2e9 || d < -2e9) return 0;
    return (int)d;
}
