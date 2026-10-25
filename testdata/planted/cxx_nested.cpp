int nested_unenc_bad(int n) {
    std::nested_exception x;
    return n;
}

int nested_unenc_ok(int n) {
    return n;
}
