void epoll_create_unenc_bad(void) {
    epoll_create();
}

int epoll_create_unenc_ok(int n) {
    return n;
}
