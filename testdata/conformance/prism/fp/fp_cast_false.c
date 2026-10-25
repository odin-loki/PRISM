/* PRISM conformance task fp/fp_cast_false.c: expected false (no-fp-cast) */
int fp_cast_false(int x) {
    double d = x * 1000.0;
    return (int)d; /* |d| > 2^31 for |x| > 2147483 */
}
