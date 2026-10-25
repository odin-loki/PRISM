int asconst_bad(void) {
    int x = 1;
    auto &c = std::as_const(x);
    const_cast<int&>(c) = 2;
    return x;
}
int asconst_ok(void) {
    int x = 1;
    auto &c = std::as_const(x);
    return c;
}
