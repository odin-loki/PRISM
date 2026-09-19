void set_find_bad(void) {
    std::set<int> s;
    auto it = s.find(1);
    (void)*it;
}
void set_find_ok(void) {
    std::set<int> s;
    auto it = s.find(1);
    if (it == s.end()) return;
    (void)*it;
}
