int tthread_g;

void tthread_bad(void) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tthread_g = 1;
}

void tthread_ok(void) {
    std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    tthread_g = 1;
}
