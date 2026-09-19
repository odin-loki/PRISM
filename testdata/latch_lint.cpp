void latch_bad(void) {
    std::latch l(1);
    l.wait();
}

void latch_ok(void) {
    std::latch l(1);
    l.count_down();
    l.wait();
}
