int getsockopt(int fd, int level, int optname, void *optval, void *optlen);

void getsockopt_bad(void) {
    getsockopt(0,0,0,0,0);
}

void getsockopt_ok(void) {
    if (getsockopt(0,0,0,0,0)!=0)
        return;
}
