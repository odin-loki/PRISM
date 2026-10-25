int slide_bad(int *p) {
    auto v = p | std::views::slide(2);
    return v[9];
}
int slide_ok(int *p) {
    auto v = p | std::views::slide(2);
    return v[0];
}
