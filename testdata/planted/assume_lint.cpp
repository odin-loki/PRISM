int assume_bad(int n) {
    [[assume(false)]];
    return n;
}

int assume_ok(int n) {
    if (n <= 0)
        return 0;
    [[assume(n > 0)]];
    return n;
}
