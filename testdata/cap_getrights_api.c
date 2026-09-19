int cap_getrights(int fd, void *rights);

void capgr_bad(void) {
    cap_getrights(0, 0);
}

void capgr_ok(void) {
    if (cap_getrights(0, 0) != 0)
        return;
}
