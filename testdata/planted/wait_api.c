int waitpid(int pid, int *status, int options);

void waitpid_bad(void) {
    waitpid(-1, 0, 0);
}

void waitpid_ok(void) {
    if (waitpid(-1, 0, 0) < 0)
        return;
}
