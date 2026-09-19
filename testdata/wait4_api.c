int wait4(int pid, int *status, int options, void *rusage);

void wait4_bad(void) {
    wait4(-1, 0, 0, 0);
}

void wait4_ok(void) {
    if (wait4(-1, 0, 0, 0) < 0)
        return;
}
