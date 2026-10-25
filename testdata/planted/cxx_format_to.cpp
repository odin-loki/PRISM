int fmtto_unenc_bad(int n) {
    (void)std::format_to(n);
    return n;
}

int fmtto_unenc_ok(int n) {
    return n;
}
