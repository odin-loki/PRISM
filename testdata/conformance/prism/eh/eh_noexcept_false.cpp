// PRISM conformance task eh/eh_noexcept_false.cpp: expected false (no-uncaught)
struct Err {
    int code;
};
static int check(int x) {
    if (x > 10) throw Err{x};
    return x;
}
int eh_noexcept_false(int x) noexcept {
    return check(x); /* x > 10: the exception reaches the noexcept boundary */
}
