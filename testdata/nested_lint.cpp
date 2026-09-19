void nested_bad(void) {
    std::throw_with_nested(std::runtime_error("x"));
}
void nested_ok(void) {
    try { std::throw_with_nested(std::runtime_error("x")); }
    catch (...) { std::rethrow_if_nested(std::current_exception()); }
}
