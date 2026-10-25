// PRISM conformance task overflow/cast_trunc_true.c: expected true (no-overflow)
int cast_trunc_true(long long a) {
    int x = (int)(a & 0xFFFF);
    return x + 1;
}
