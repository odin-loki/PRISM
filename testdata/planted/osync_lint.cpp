void osync_bad(void) {
    std::osyncstream oss(std::cout);
    oss << 1;
}

void osync_ok(void) {
    std::osyncstream oss(std::cout);
    oss << 1;
    oss.emit();
}
