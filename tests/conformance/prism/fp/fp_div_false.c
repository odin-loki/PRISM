/* PRISM conformance task fp/fp_div_false.c: expected false (no-fp-cast) */
int fp_div_false(int a, int b) {
    double q = (double)a / (double)b; /* b == 0: infinite */
    return (int)q;
}
