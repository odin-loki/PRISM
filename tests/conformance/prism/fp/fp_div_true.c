/* PRISM conformance task fp/fp_div_true.c: expected true (no-fp-cast) */
int fp_div_true(int a, int b) {
    if (b == 0 || (a == -2147483647 - 1 && b == -1)) return 0;
    double q = (double)a / (double)b;
    return (int)q;
}
