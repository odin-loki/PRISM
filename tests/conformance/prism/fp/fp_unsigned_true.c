/* PRISM conformance task fp/fp_unsigned_true.c: expected true (no-fp-cast) */
unsigned fp_unsigned_true(int x) {
    float f = (float)x;
    if (f < 0.0f) return 0;
    return (unsigned)f;
}
