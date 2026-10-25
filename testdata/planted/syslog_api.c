int syslog(int priority, const char *msg);

void syslog_bad(void) {
    syslog(0,0);
}

void syslog_ok(void) {
    if (syslog(0,0)!=-1)
        return;
}
