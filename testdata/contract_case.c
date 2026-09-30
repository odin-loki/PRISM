/* Contract keywords are case-insensitive, in // clauses and in ACSL. */
int inc_caps(int x) {
    // Requires: x < 100
    // ENSURES: result == x + 1
    return x + 1;
}

/* False twin: the capitalised ensures is checked, and it does not hold. */
int inc_caps_bad(int x) {
    // Requires: x < 100
    // Ensures: result == x
    return x + 1;
}

/*@ Requires x > -2147483647;
    Ensures \result >= 0; */
int abs_caps(int x) {
    if (x < 0) return -x;
    return x;
}
