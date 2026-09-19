void weak_bad(void) {
    std::weak_ptr<int> w;
    auto p = w.lock();
    *p = 1;
}

void weak_ok(void) {
    std::weak_ptr<int> w;
    auto p = w.lock();
    if (!p) return;
    *p = 1;
}
