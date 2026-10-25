void task_bad(void) {
    std::task<int> t = make_task();
}

void task_ok(void) {
    int v = sync_wait(make_task());
    (void)v;
}
