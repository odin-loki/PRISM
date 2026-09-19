int msgctl(int msqid, int cmd, void *buf);

void msgctl_bad(void) {
    msgctl(0, 0, 0);
}

void msgctl_ok(void) {
    if (msgctl(0, 0, 0)!=-1)
        return;
}
