int stdmeta_unenc_bad(int n) {
    std::meta::reflect(n);
    return n;
}

int stdmeta_unenc_ok(int n) {
    return n;
}
