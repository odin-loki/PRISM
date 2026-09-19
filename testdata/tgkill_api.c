int tgkill(int tgid, int tid, int sig);

void tgkill_bad(void) {
    tgkill(0, 0, 0);
}

void tgkill_ok(void) {
    if (tgkill(0, 0, 0)!=-1)
        return;
}
