int socket(int domain, int type, int protocol);
int write(int fd, const void *buf, unsigned n);

int socket_bad(void) {
    int s = socket(2, 1, 0);
    write(s, 0, 0);
    return s;
}

int socket_ok(void) {
    int s = socket(2, 1, 0);
    if (s < 0)
        return -1;
    write(s, 0, 0);
    return s;
}
