int rinested_unenc_bad(int n) {
    rethrow_if_nested();
    return n;
}

int rinested_unenc_ok(int n) {
    return n;
}
