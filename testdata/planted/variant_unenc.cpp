int variant_unenc_bad(int n) {
    variant<int, char> v;
    (void)v;
    return n;
}

int variant_unenc_ok(int n) {
    return n;
}
