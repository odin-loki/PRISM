int sched_setaffinity(int pid, unsigned long cpusetsize, const void *mask);
int sched_getaffinity(int pid, unsigned long cpusetsize, void *mask);

void sched_bad(void) {
    sched_setaffinity(0,0,0);
}

void sched_ok(void) {
    if (sched_setaffinity(0,0,0)!=0)
        return;
}
