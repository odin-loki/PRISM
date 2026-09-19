int quotactl(int cmd, const char *special, int id, void *addr);

void quotactl_bad(void) {
    quotactl(0,0,0,0);
}

void quotactl_ok(void) {
    if (quotactl(0,0,0,0)!=0)
        return;
}
