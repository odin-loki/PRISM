int byteswap_unenc_bad(int n) {
    std::byteswap(n);
    return n;
}

int byteswap_unenc_ok(int n) {
    return n;
}
