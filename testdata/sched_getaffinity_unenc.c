void sched_getaffinity_unenc_bad(void) {
    sched_getaffinity();
}

int sched_getaffinity_unenc_ok(int n) {
    return n;
}
