void worker_async(void) {
}

void async_bad(void) {
    std::async(worker_async);
}

void async_ok(void) {
    auto fut = std::async(worker_async);
    fut.get();
}
