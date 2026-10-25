// PRISM conformance task regress/cast_alias_true.c: expected true (no-overflow)
// regression: types: a cast of a value does not retype the value itself
int cast_alias_true(int x) {
    unsigned y = (unsigned)x;
    if (x == 2147483647) return 0;
    return x + 1 + (int)(y & 0u);
}
