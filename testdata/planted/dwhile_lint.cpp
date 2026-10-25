int pos(int x) { return x > 0; }
int dwhile_bad(int *p) {
    auto v = p | std::views::drop_while(pos);
    return v[9];
}
int dwhile_ok(int *p) {
    auto v = p | std::views::drop_while(pos);
    return v[0];
}
