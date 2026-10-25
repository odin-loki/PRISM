int atomic_ref_lt_unenc_bad(int n) {
    atomic_ref<int> a(n); return n;
}

int atomic_ref_lt_unenc_ok(int n) {
    return n;
}
