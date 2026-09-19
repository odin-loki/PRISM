int sigprocmask(int how, const void *set, void *oldset);

void sigprocmask_bad(void) {
    sigprocmask(0, 0, 0);
}

void sigprocmask_ok(void) {
    if (sigprocmask(0, 0, 0) != 0)
        return;
}
