int epoll_pwait(int epfd, void *events, int maxevents, int timeout, const void *sigmask);

void epollpw_bad(void) {
    epoll_pwait(0, 0, 0, 0, 0);
}

void epollpw_ok(void) {
    if (epoll_pwait(0, 0, 0, 0, 0) < 0)
        return;
}
