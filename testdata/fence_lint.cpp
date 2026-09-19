int g;
void fence_bad(void) {
    g = 1;
    std::atomic_thread_fence(std::memory_order_seq_cst);
}
void fence_ok(void) {
    std::atomic<int> a{0};
    a.store(1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}
