int bitfield_bad(int n) {
    struct { int a:3; } bf;
    (void)bf;
    return n;
}

int bitfield_ok(int n) {
    int x = n;
    return x;
}
