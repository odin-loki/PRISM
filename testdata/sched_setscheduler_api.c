int sched_setscheduler(int pid, int policy, const void *param);

void setsched_bad(void) {
    sched_setscheduler(0,0,0);
}

void setsched_ok(void) {
    if (sched_setscheduler(0,0,0)!=-1)
        return;
}
