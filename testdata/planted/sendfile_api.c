int sendfile(int out_fd, int in_fd, void *offset, unsigned count);

void sendfile_bad(void) {
    sendfile(0,0,0,0);
}

void sendfile_ok(void) {
    if (sendfile(0,0,0,0)<0)
        return;
}
