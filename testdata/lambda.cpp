int lambda_bad(int n) {
    auto f = [](int x) { return x; };
    return f(n);
}

int lambda_ok(int n) {
    return n;
}
