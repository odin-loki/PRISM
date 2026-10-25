void cvany_bad(void) {
    std::condition_variable_any cv;
    cv.wait();
}

void cvany_ok(void) {
    std::condition_variable_any cv;
    std::mutex m;
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, []{ return true; });
}
