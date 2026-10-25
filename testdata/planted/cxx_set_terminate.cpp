int setterm_unenc_bad(int n) {
    std::set_terminate();
    return n;
}

int setterm_unenc_ok(int n) {
    return n;
}
