int any_unenc_bad(int n) {
    std::any a;
    (void)any_cast<int>(a);
    return n;
}

int any_unenc_ok(int n) {
    return n;
}
