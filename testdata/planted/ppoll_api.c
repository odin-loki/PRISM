int ppoll(void *fds, unsigned nfds, const void *ts, const void *sigmask);

void ppoll_bad(void) {
    ppoll(0, 0, 0, 0);
}

void ppoll_ok(void) {
    if (ppoll(0, 0, 0, 0) < 0)
        return;
}
