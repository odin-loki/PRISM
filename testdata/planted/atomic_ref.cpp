int atomic_ref_unenc_bad(int n) {
    std::atomic_ref<int> r;
    return n;
}

int atomic_ref_unenc_ok(int n) {
    return n;
}
