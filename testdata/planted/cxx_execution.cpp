int execution_unenc_bad(int n) {
    std::execution::sequenced_policy p;
    return n;
}

int execution_unenc_ok(int n) {
    return n;
}
