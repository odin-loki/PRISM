void stmutex_bad(void) {
    std::shared_timed_mutex m;
    m.lock();
}
void stmutex_ok(void) {
    std::shared_timed_mutex m;
    m.lock();
    m.unlock();
}
