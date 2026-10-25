// PRISM conformance task eh/eh_cleanup_true.cpp: expected true (no-div0)
struct Guard {
    int* p;
    ~Guard() { *p += 1; }
};
struct Err {
    int code;
};
static int check(int x) {
    if (x > 10) throw Err{x};
    return x;
}
int eh_cleanup_true(int x) {
    int n = 0;
    try {
        Guard g{&n};
        check(x);
    } catch (...) {
        return 100 / n; /* the destructor ran: n == 1 */
    }
    return 100 / n;
}
