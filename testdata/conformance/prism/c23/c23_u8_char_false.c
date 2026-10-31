// PRISM conformance task c23/c23_u8_char_false.c: expected false (no-oob)
int c23_u8_char_false(int x) {
    const char a[2] = {u8'a', u8'b'};
    if (x < 0 || x > 2) return 0;
    return a[x];
}
