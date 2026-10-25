int getmntinfo(void *mntbufp, int flags);

void getmntinfo_bad(void) {
    getmntinfo(0, 0);
}

void getmntinfo_ok(void) {
    if (getmntinfo(0, 0) < 0)
        return;
}
