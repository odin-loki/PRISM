/* PRISM conformance task fp/fp_round_false.c: expected false (no-div0) */
int fp_round_false(int x) {
    if (x < 0 || x > 100000000) return 0;
    float f = (float)x; /* 16777217 rounds to 16777216 */
    return 100 / ((int)f - x + 1);
}
