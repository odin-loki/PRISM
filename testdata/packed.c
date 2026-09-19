int packed_bad(int n) {
    struct __attribute__((packed)) { int a; char b; } s;
    s.a = n;
    return s.a + (int)s.b;
}

int packed_ok(int n) {
    return n;
}
