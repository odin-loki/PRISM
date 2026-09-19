void handler(int);

void signal_bad(void) {
    signal(SIGINT, handler);
}

void signal_ok(void) {
    signal(SIGINT, SIG_IGN);
}

void signal_dfl_ok(void) {
    signal(SIGINT, SIG_DFL);
}
