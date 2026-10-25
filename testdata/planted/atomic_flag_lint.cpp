void aflag_bad(void) {
    std::atomic_flag f;
    f.test_and_set();
}

void aflag_ok(void) {
    std::atomic_flag f;
    f.clear();
    f.test_and_set();
}
