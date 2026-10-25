// PRISM conformance task overflow/bool_arith_false.c: expected false (no-overflow)
int bool_arith_false(int a, int b) {
    return (a > b) + a;
}
