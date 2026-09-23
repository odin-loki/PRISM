// PRISM conformance task eh/eh_type_false.cpp: expected false (no-uncaught)
struct Err {
    int code;
};
static int check(int x) {
    if (x > 10) throw Err{x};
    return x;
}
int eh_type_false(int x) noexcept {
    try {
        return check(x);
    } catch (int) { /* Err is not caught here */
        return 0;
    }
}
