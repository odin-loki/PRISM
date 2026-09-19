int flat_multiset_bad(void) {
    std::flat_multiset<int> s;
    return *s.find(0);
}

int flat_multiset_ok(void) {
    std::flat_multiset<int> s;
    auto it = s.find(0);
    if (it != s.end())
        return *it;
    return 0;
}
