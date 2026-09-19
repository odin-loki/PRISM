int quotactl_fd(int fd, unsigned cmd, int id, void *addr);

void qcfd_bad(void) {
    quotactl_fd(0, 0, 0, 0);
}

void qcfd_ok(void) {
    if (quotactl_fd(0, 0, 0, 0)!=-1)
        return;
}
