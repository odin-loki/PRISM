int flat_map_bad(void) {
    std::flat_map<int,int> m;
    return m.at(0);
}

int flat_map_ok(void) {
    std::flat_map<int,int> m;
    if (m.contains(0))
        return m.at(0);
    return 0;
}
