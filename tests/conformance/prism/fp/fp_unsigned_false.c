/* PRISM conformance task fp/fp_unsigned_false.c: expected false (no-fp-cast) */
unsigned fp_unsigned_false(int x) {
    float f = (float)x - 0.5f;
    if (f < -2.0f) return 0;
    return (unsigned)(f - 1.0f); /* x == 0: -1.5 truncates to -1 */
}
