void fanotify_mark_unenc_bad(void) {
    fanotify_mark();
}

int fanotify_mark_unenc_ok(int n) {
    return n;
}
