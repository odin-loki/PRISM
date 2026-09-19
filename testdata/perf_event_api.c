int perf_event_open(void *attr, int pid, int cpu, int group_fd,
                    unsigned long flags);
int close(int fd);

void perf_event_bad(void) {
    int fd=perf_event_open(0,0,0,0,0);
    close(fd);
}

void perf_event_ok(void) {
    int fd=perf_event_open(0,0,0,0,0);
    if (fd<0)
        return;
    close(fd);
}
