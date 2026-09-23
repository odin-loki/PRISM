// PRISM conformance task overflow/cast_trunc_false.c: expected false (no-overflow)
// the cast itself is implementation-defined, the following +1 overflows
int cast_trunc_false(long long a) {
    int x = (int)a;
    return x + 1;
}
