// PRISM conformance task div/div_const_ok_false.c: expected false (no-div0)
int div_const_ok_false(int a) {
    int d = a & 3;
    return 100 / d;
}
