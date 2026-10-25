int kill(int pid, int sig);

void kill_bad(void) {
    kill(1, 9);
}

void kill_ok(void) {
    if (kill(1, 9) != 0)
        return;
}
