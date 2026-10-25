int sigaction(int signum, const void *act, void *oldact);

void sigaction_bad(void) {
    sigaction(0, 0, 0);
}

void sigaction_ok(void) {
    if (sigaction(0, 0, 0) != 0)
        return;
}
