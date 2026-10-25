long sysconf(int name);

void sysconf_bad(void) {
    sysconf(0);
}

void sysconf_ok(void) {
    if (sysconf(0)<0)
        return;
}
