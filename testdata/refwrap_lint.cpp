int refwrap_bad(void) {
    int x = 1;
    return std::ref(x);
}
int refwrap_ok(int& x) {
    auto r = std::ref(x);
    return r.get();
}
