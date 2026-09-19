void fwd_bad(void) {
    std::forward_list<int> l;
    int x=l.front();
    (void)x;
}

void fwd_ok(void) {
    std::forward_list<int> l;
    if (l.empty()) return;
    int x=l.front();
    (void)x;
}
