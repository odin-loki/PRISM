void list_front_bad(void) {
    std::list<int> l;
    l.front();
}

void list_front_ok(void) {
    std::list<int> l;
    if (l.empty()) return;
    l.front();
}
