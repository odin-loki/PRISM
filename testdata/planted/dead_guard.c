int dead_guard_bad(unsigned n) {
    if (n < 0)
        return 0;
    return 1;
}

int dead_guard_ok(int n) {
    if (n < 0)
        return 0;
    return 1;
}
