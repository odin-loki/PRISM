int expected_error_bad(void) {
    expected<int, int> e;
    return e.error();
}

int expected_error_ok(void) {
    expected<int, int> e;
    if (!e)
        return e.error();
    return 0;
}
