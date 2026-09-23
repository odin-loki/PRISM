/* PRISM conformance task fp/fp_div_true.c: expected true (no-fp-cast) */
int fp_div_true(short a, short b) {
    if (b == 0) return 0;
    float q = (float)a / (float)b; /* |q| <= 32768 */
    return (int)q;
}
