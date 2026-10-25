int tview_id(int x) { return x; }
int tview_bad(int *p) {
    auto v = p | std::views::transform(tview_id);
    return v[9];
}
int tview_ok(int *p) {
    auto v = p | std::views::transform(tview_id);
    return v[0];
}
