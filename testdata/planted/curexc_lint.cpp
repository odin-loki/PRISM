void curexc_bad(void) {
    std::rethrow_exception(std::current_exception());
}
void curexc_ok(void) {
    auto e = std::current_exception();
    if (!e) return;
    std::rethrow_exception(e);
}
