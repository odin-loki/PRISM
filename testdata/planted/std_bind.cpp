int bind_add(int a, int b) {
    return a + b;
}

int std_bind_bad(int n) {
    std::bind(bind_add, n, 1);
    return n;
}

int std_bind_ok(int n) {
    return n;
}
