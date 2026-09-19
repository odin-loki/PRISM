int cachestat(int fd, void *cstat_range, void *cstat, unsigned int flags);

void cachestat_bad(void) {
    cachestat(0,0,0,0);
}

void cachestat_ok(void) {
    if (cachestat(0,0,0,0)!=-1)
        return;
}
