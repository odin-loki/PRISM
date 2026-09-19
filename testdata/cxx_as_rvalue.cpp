int asrval_unenc_bad(int n) {
    std::views::as_rvalue x;
    return n;
}

int asrval_unenc_ok(int n) {
    return n;
}
