int packaged_unenc_bad(int n) {
    std::packaged_task<int()> t;
    return n;
}

int packaged_unenc_ok(int n) {
    return n;
}
