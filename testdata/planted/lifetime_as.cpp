int lifetime_unenc_bad(int n) {
    (void)std::start_lifetime_as(n);
    return n;
}

int lifetime_ok(int n) {
    return n;
}
