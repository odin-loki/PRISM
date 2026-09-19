void sigqueueinfo_unenc_bad(void) {
    rt_sigqueueinfo();
}

int sigqueueinfo_unenc_ok(int n) {
    return n;
}
