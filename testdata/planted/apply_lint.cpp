void apply_bad(void) {
    std::function<int(int,int)> f;
    std::apply(f, std::tuple<int,int>{1,2});
}
void apply_ok(void) {
    std::function<int(int,int)> f;
    if (!f) return;
    std::apply(f, std::tuple<int,int>{1,2});
}
