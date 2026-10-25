int nested_fn_bad(int n) {
    void inner(void) {
    }
    return n;
}

int nested_fn_ok(int n) {
    return n;
}
