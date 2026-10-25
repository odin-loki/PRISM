int sigwait(const void *set, int *sig);

void sigwait_bad(void) {
    sigwait(0, 0);
}

void sigwait_ok(void) {
    if (sigwait(0, 0) != 0)
        return;
}
