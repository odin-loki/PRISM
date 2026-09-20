void inotify_init1_unenc_bad(void) {
    inotify_init1();
}

int inotify_init1_unenc_ok(int n) {
    return n;
}
