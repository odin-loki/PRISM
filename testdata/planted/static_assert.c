int static_assert_ok(int n) {
    _Static_assert(1, "ok");
    static_assert(1, "ok");
    return n;
}
