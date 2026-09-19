void packaged_bad(void) {
    std::packaged_task<void()> t;
    t();
}

void packaged_ok(void) {
    std::packaged_task<void()> t;
    if (t)
        t();
}
