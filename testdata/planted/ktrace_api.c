int ktrace(const char *tracefile, int ops, int trpoints, int pid);

void ktrace_bad(void) {
    ktrace(0, 0, 0, 0);
}

void ktrace_ok(void) {
    if (ktrace(0, 0, 0, 0) != 0)
        return;
}
