int call_once_unenc_bad(int n) {
    std::call_once();
    return n;
}

int call_once_unenc_ok(int n) {
    return n;
}
