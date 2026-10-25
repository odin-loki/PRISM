void membarrier_unenc_bad(void) {
    membarrier();
}

int membarrier_unenc_ok(int n) {
    return n;
}
