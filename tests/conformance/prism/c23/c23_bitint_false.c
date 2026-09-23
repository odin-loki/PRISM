// PRISM conformance task c23/c23_bitint_false.c: expected false (no-overflow)
// signed _BitInt(40) overflow is undefined
long long c23_bitint_false(int a) {
    _BitInt(40) x = a;
    _BitInt(40) y = x * 1000;
    return (long long)y;
}
