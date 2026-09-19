int pdgetpid(int fd, int *pidp);

void pdgetpid_bad(void) {
    pdgetpid(0, 0);
}

void pdgetpid_ok(void) {
    if (pdgetpid(0, 0) != 0)
        return;
}
