void barrier_bad(void) {
    std::barrier b(1);
    b.arrive();
}

void barrier_ok(void) {
    std::barrier b(1);
    b.arrive_and_wait();
}
