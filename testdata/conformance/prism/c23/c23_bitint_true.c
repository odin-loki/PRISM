// PRISM conformance task c23/c23_bitint_true.c: expected true (no-overflow)
int c23_bitint_true(int a) {
    _BitInt(40) x = a;
    _BitInt(40) y = x * 100;
    return (int)(y / 1000);
}
