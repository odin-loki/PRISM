int revoke(const char *path);

void revoke_bad(void) {
    revoke("/dev/tty");
}

void revoke_ok(void) {
    if (revoke("/dev/tty") != 0)
        return;
}
