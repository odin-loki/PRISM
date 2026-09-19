void invoke_bad(void) {
    std::function<void()> f;
    std::invoke(f);
}
void invoke_ok(void) {
    std::function<void()> f;
    if (!f) return;
    std::invoke(f);
}
