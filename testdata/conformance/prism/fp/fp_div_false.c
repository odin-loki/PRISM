/* PRISM conformance task fp/fp_div_false.c: expected false (no-fp-cast) */
int fp_div_false(short a, short b) {
    float q = (float)a / (float)b; /* b == 0: infinite */
    return (int)q;
}
