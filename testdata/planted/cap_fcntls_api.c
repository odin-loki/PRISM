int cap_fcntls_limit(int fd, unsigned rights);

void capfcntls_bad(void) {
    cap_fcntls_limit(0, 0);
}

void capfcntls_ok(void) {
    if (cap_fcntls_limit(0, 0) != 0)
        return;
}
