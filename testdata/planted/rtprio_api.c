int rtprio(int function, int pid, void *rtp);
int rtprio_thread(int function, int lwpid, void *rtp);

void rtprio_bad(void) {
    rtprio(0, 0, 0);
}

void rtprio_ok(void) {
    if (rtprio(0, 0, 0) != 0)
        return;
}
