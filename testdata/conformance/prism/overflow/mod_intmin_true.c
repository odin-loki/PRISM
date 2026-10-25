// PRISM conformance task overflow/mod_intmin_true.c: expected true (no-overflow)
int mod_intmin_true(int a, int b) {
    if (b == 0 || b == -1) return 0;
    return a % b;
}
