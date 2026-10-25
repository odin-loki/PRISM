int chunk_bad(int *p) {
    auto v = p | std::views::chunk(2);
    return v[9];
}
int chunk_ok(int *p) {
    auto v = p | std::views::chunk(2);
    return v[0];
}
