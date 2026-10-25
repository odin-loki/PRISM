void syserr_bad(void) {
    std::system_error e(std::error_code());
    (void)e.what();
}
void syserr_ok(void) {
    std::system_error e(std::error_code());
    if (e.code()) return;
    (void)e.what();
}
