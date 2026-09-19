void mset_find_bad(void) {
    std::multiset<int> s;
    auto it=s.find(1);
    (void)*it;
}
void mset_find_ok(void) {
    std::multiset<int> s;
    auto it=s.find(1);
    if (it==s.end()) return;
    (void)*it;
}
