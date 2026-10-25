int expected_null_bad(void) {
    expected<int, int> e;
    return *e;
}

int expected_null_ok(void) {
    expected<int, int> e;
    if (e.has_value())
        return *e;
    return 0;
}
