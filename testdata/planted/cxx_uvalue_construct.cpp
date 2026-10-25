int uvalue_unenc_bad(int n) {
    std::uninitialized_value_construct();
    return n;
}

int uvalue_unenc_ok(int n) {
    return n;
}
