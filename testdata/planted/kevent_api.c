int kevent(int kq, const void *changelist, int nchanges, void *eventlist, int nevents, const void *timeout);

void kevent_bad(void) {
    kevent(0, 0, 0, 0, 0, 0);
}

void kevent_ok(void) {
    if (kevent(0, 0, 0, 0, 0, 0) < 0)
        return;
}
