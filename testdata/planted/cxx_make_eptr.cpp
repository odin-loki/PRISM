int mkeptr_unenc_bad(int n) {
    std::make_exception_ptr();
    return n;
}

int mkeptr_unenc_ok(int n) {
    return n;
}
