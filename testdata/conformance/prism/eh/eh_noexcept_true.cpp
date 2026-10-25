// PRISM conformance task eh/eh_noexcept_true.cpp: expected true (no-uncaught)
struct Err {
    int code;
};
static int check(int x) {
    if (x > 10) throw Err{x};
    return x;
}
int eh_noexcept_true(int x) noexcept {
    if (x > 10) return 0;
    return check(x);
}
