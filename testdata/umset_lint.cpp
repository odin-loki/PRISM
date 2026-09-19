void umset_find_bad(void) {
    std::unordered_multiset<int> s;
    auto it=s.find(1);
    (void)*it;
}
void umset_find_ok(void) {
    std::unordered_multiset<int> s;
    auto it=s.find(1);
    if (it==s.end()) return;
    (void)*it;
}
