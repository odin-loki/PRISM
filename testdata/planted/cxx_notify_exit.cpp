int nexit_unenc_bad(int n) {
    notify_all_at_thread_exit();
    return n;
}

int nexit_unenc_ok(int n) {
    return n;
}
