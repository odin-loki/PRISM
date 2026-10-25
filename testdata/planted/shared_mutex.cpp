void shared_mutex_bad(void) {
    shared_mutex m;
    m.lock();
}

void shared_mutex_ok(void) {
    shared_mutex m;
    lock_guard<shared_mutex> g(m);
}
