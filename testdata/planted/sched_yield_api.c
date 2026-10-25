int sched_yield(void);

void yield_bad(void) {
    sched_yield();
}

void yield_ok(void) {
    if (sched_yield()!=-1)
        return;
}
