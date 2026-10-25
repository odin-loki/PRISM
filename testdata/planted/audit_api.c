int auditon(int cmd, void *data, unsigned len);

void audit_bad(void) {
    auditon(0, 0, 0);
}

void audit_ok(void) {
    if (auditon(0, 0, 0) != 0)
        return;
}
