int rangefor_unenc_bad(int n) {
    int xs[2];
    for (int x : xs)
        (void)x;
    return n;
}

int rangefor_unenc_ok(int n) {
    return n;
}
