int indirect_bad(void) {
    std::indirect<int> p;
    return *p;
}

int indirect_ok(void) {
    std::indirect<int> p(1);
    return *p;
}
