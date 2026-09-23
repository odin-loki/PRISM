// PRISM conformance task eh/eh_type_true.cpp: expected true (no-uncaught)
struct Err {
    int code;
};
static int check(int x) {
    if (x > 10) throw Err{x};
    return x;
}
int eh_type_true(int x) noexcept {
    try {
        return check(x);
    } catch (const Err&) {
        return 0;
    }
}
