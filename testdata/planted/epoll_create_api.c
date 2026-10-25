int epoll_create(int size);

void epollc_bad(void) {
    epoll_create(0);
}

void epollc_ok(void) {
    if (epoll_create(0)!=-1)
        return;
}
