int flat_multimap_bad(void) {
    std::flat_multimap<int, int> m;
    return *m.find(0);
}

int flat_multimap_ok(void) {
    std::flat_multimap<int, int> m;
    auto it = m.find(0);
    if (it != m.end())
        return it->second;
    return 0;
}
