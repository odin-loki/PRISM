int pipe(int fds[2]);

void pipe_bad(void) {
    int fds[2];
    pipe(fds);
    (void)fds[0];
}

void pipe_ok(void) {
    int fds[2];
    if (pipe(fds) < 0)
        return;
}
