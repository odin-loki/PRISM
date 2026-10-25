void uset_find_bad(void) {
    std::unordered_set<int> s;
    auto it=s.find(1);
    (void)*it;
}
void uset_find_ok(void) {
    std::unordered_set<int> s;
    auto it=s.find(1);
    if (it==s.end()) return;
    (void)*it;
}
