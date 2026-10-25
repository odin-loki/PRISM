void *kinfo_getproc(int pid);

void kinfo_bad(void) {
    kinfo_getproc(0);
}

void kinfo_ok(void) {
    if (kinfo_getproc(0) == 0)
        return;
}
