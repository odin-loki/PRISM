void tmutex_bad(void) {
    std::timed_mutex m;
    m.lock();
}

void tmutex_ok(void) {
    std::timed_mutex m;
    if (!m.try_lock()) return;
    m.unlock();
}
