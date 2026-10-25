void mkeptr_bad(void) {
    std::rethrow_exception(std::make_exception_ptr(1));
}
void mkeptr_ok(void) {
    auto e = std::make_exception_ptr(1);
    if (!e) return;
    std::rethrow_exception(e);
}
