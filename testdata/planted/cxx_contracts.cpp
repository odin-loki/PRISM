int cassert_unenc_bad(int n) {
    contract_assert(n);
    return n;
}

int cassert_unenc_ok(int n) {
    return n;
}
