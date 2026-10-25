void rmutex_bad(void) {
    std::recursive_mutex m;
    m.lock();
}

void rmutex_ok(void) {
    std::recursive_mutex m;
    m.lock();
    m.unlock();
}
