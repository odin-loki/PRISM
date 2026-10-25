// PRISM conformance task overflow/preinc_true.c: expected true (no-overflow)
int preinc_true(int a) {
    if (a == 2147483647) return a;
    return ++a;
}
