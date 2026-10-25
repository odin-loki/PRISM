int sendmsg(int fd, const void *msg, int flags);

void sendmsg_bad(void) {
    sendmsg(0, 0, 0);
}

void sendmsg_ok(void) {
    if (sendmsg(0, 0, 0) < 0)
        return;
}
