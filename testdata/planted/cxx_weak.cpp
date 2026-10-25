int weak_ptr_unenc_bad(int n) {
    std::weak_ptr<int> w;
    return n;
}

int weak_ptr_unenc_ok(int n) {
    return n;
}
