// PRISM conformance task regress/cast_alias_false.c: expected false (no-overflow)
// regression: types: a cast of a value does not retype the value itself
int cast_alias_false(int x) {
    unsigned y = (unsigned)x;
    return x + 1 + (int)(y & 0u);
}
