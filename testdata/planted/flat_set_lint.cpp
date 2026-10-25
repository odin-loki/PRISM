int flat_set_bad(void) {
    std::flat_set<int> s;
    return *s.find(0);
}

int flat_set_ok(void) {
    std::flat_set<int> s;
    auto it = s.find(0);
    if (it != s.end())
        return *it;
    return 0;
}
