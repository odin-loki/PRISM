int socketpair(void);

void spair_bad(void) {
    socketpair();
}

void spair_ok(void) {
    if (socketpair()!=-1)
        return;
}
