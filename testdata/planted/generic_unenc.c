int generic_unenc_bad(int n) {
    return _Generic(n, int: 1, default: 0);
}

int generic_unenc_ok(int n) {
    return n;
}
