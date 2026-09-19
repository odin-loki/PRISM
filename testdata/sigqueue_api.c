int sigqueue(int pid, int sig, int value);

void sigqueue_bad(void) {
    sigqueue(1, 0, 0);
}

void sigqueue_ok(void) {
    if (sigqueue(1, 0, 0) != 0)
        return;
}
