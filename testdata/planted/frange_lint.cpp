int frange_bad(int *p) {
    std::vector<int> v(std::from_range, p, p + 4);
    return v[9];
}
int frange_ok(int *p) {
    std::vector<int> v(std::from_range, p, p + 4);
    return v[0];
}
