void cond_wait_bad(void) {
    condition_variable cv;
    mutex m;
    unique_lock<mutex> lk(m);
    cv.wait(lk);
}

void cond_wait_ok(void) {
    condition_variable cv;
    mutex m;
    unique_lock<mutex> lk(m);
    int ready = 1;
    cv.wait(lk, [&]{ return ready; });
}
