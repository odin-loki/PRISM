int launder_unenc_bad(int n) {
    (void)std::launder(n);
    return n;
}

int launder_ok(int n) {
    return n;
}
