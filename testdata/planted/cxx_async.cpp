int async_unenc_bad(int n) {
    (void)std::async();
    return n;
}

int async_unenc_ok(int n) {
    return n;
}
