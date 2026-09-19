int twhile_pos(int x) { return x > 0; }
int twhile_bad(int *p) {
    auto v = p | std::views::take_while(twhile_pos);
    return v[9];
}
int twhile_ok(int *p) {
    auto v = p | std::views::take_while(twhile_pos);
    return v[0];
}
