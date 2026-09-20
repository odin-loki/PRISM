int trycatch_unenc_bad(int n) {
    try {
        throw 1;
    } catch (int e) {
        (void)e;
    }
    return n;
}

int trycatch_unenc_ok(int n) {
    return n;
}
