int fn_ref_unenc_bad(int n) {
    std::function_ref<int(int)> r;
    return n;
}

int fn_ref_unenc_ok(int n) {
    return n;
}
