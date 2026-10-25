void errc_bad(void) {
    std::error_code ec;
    (void)ec.message();
}

void errc_ok(void) {
    std::error_code ec;
    if (ec) return;
    (void)ec.message();
}
