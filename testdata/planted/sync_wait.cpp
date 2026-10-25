void sync_wait_bad(void) {
    std::execution::sync_wait(snd);
}

void sync_wait_ok(void) {
    int v = std::execution::sync_wait(snd);
    (void)v;
}
