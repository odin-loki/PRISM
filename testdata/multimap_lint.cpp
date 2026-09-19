void mmap_find_bad(void) {
    std::multimap<int,int> m;
    auto it=m.find(1);
    (void)it->second;
}
void mmap_find_ok(void) {
    std::multimap<int,int> m;
    auto it=m.find(1);
    if (it==m.end()) return;
    (void)it->second;
}
