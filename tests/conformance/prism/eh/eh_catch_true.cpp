// PRISM conformance task eh/eh_catch_true.cpp: expected true (no-div0)
struct Err {
    int code;
};
static int check(int x) {
    if (x > 10) throw Err{x};
    return x;
}
int eh_catch_true(int x) {
    try {
        return check(x);
    } catch (const Err& e) {
        return 100 / (e.code - 10); /* code > 10 */
    }
}
