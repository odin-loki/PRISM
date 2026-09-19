int sendmmsg(int fd, void *msgvec, unsigned vlen, int flags);
int recvmmsg(int fd, void *msgvec, unsigned vlen, int flags, void *timeout);

void sendmmsg_bad(void) {
    sendmmsg(0,0,0,0);
}

void sendmmsg_ok(void) {
    if (sendmmsg(0,0,0,0)<0)
        return;
}
