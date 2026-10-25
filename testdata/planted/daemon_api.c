int daemon(int nochdir, int noclose);
void setproctitle(const char *fmt, ...);

void daemon_bad(void) {
    daemon(0, 0);
}

void daemon_ok(void) {
    if (daemon(0, 0) != 0)
        return;
}
