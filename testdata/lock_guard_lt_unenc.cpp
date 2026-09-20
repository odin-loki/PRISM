int lock_guard_lt_unenc_bad(int n) {
    lock_guard<mutex> g; return n;
}

int lock_guard_lt_unenc_ok(int n) {
    return n;
}
