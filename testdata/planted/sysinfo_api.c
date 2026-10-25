int sysinfo(void);

void sysinfo_bad(void) {
    sysinfo();
}

void sysinfo_ok(void) {
    if (sysinfo()!=-1)
        return;
}
