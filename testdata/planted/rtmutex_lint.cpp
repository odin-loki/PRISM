void rtmutex_bad(void) {
    std::recursive_timed_mutex m;
    m.lock();
}
void rtmutex_ok(void) {
    std::recursive_timed_mutex m;
    m.lock();
    m.unlock();
}
