int alignas_unenc_bad(int n) {
    _Alignas(8) int x = n;
    return x;
}

int alignas_unenc_ok(int n) {
    return n;
}
