void epoll_create1_unenc_bad(void) {
    epoll_create1();
}

int epoll_create1_unenc_ok(int n) {
    return n;
}
