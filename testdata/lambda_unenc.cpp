int lambda_unenc_bad(int n) {
    auto f = [](int x) { return x; };
    return f(n);
}

int lambda_unenc_ok(int n) {
    return n;
}
