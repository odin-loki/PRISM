void nexit_bad(void) {
    std::mutex m;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lk(m);
    std::notify_all_at_thread_exit(cv, std::move(lk));
}
void nexit_ok(void) {
    std::mutex m;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, []{ return true; });
    std::notify_all_at_thread_exit(cv, std::move(lk));
}
