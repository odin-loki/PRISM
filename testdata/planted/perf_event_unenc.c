void perf_event_unenc_bad(void) {
    perf_event_open();
}

int perf_event_unenc_ok(int n) {
    return n;
}
