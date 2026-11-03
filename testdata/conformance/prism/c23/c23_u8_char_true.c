// PRISM conformance task c23/c23_u8_char_true.c: expected true (no-oob)
int c23_u8_char_true(int x) {
    const char a[2] = {u8'a', u8'b'};
    if (x < 0 || x > 1) return 0;
    return a[x];
}
