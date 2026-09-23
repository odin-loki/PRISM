// PRISM conformance task overflow/bool_arith_true.c: expected true (no-overflow)
int bool_arith_true(int a, int b) {
    return (a > b) + (a == b) + (a < b);
}
