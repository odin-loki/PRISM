// PRISM conformance task overflow/mod_intmin_false.c: expected false (no-overflow)
// INT_MIN % -1 is undefined in C11 and later (6.5.5p6)
int mod_intmin_false(int a, int b) {
    if (b == 0) return 0;
    return a % b;
}
