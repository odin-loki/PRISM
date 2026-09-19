int copyable_unenc_bad(int n) {
    std::copyable_function<int()> f;
    return n;
}

int copyable_unenc_ok(int n) {
    return n;
}
