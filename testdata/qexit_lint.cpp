void qexit_bad(void) { std::quick_exit(1); }
void qexit_ok(void) {
    std::at_quick_exit([]{});
    std::quick_exit(0);
}
