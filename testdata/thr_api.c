int thr_kill(long id, int sig);

void thr_bad(void) {
    thr_kill(0, 0);
}

void thr_ok(void) {
    if (thr_kill(0, 0) != 0)
        return;
}
