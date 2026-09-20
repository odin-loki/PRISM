int sbind_unenc_bad(int n) {
    std::bind(bind_add, n, 1);
    return n;
}

int sbind_unenc_ok(int n) {
    return n;
}
