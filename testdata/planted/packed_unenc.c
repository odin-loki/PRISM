int packed_unenc_bad(int n) {
    struct __attribute__((packed)) { int a; char b; } s;
    s.a = n;
    return s.a;
}

int packed_unenc_ok(int n) {
    return n;
}
