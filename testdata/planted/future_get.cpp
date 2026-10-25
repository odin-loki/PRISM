void worker_fut(void) {
}

void future_get_bad(void) {
    std::future<int> fut;
    fut.get();
}

int future_get_ok(void) {
    std::future<int> f = std::async(worker_fut);
    return f.get();
}
