int ioctl(int fd, unsigned long request, ...);

void ioctl_bad(int fd) {
    ioctl(fd, 0);
}

void ioctl_ok(int fd) {
    if (ioctl(fd, 0) < 0)
        return;
}
