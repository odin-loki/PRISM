void hive_bad(void) {
    std::hive<int> h;
    auto it = h.begin();
    h.insert(1);
    (void)*it;
}

void hive_ok(void) {
    std::hive<int> h;
    h.insert(1);
    auto it = h.begin();
    (void)*it;
}
