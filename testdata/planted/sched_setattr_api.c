int sched_setattr(int pid, void *attr, unsigned flags);

void setattr_bad(void) {
    sched_setattr(0, 0, 0);
}

void setattr_ok(void) {
    if (sched_setattr(0, 0, 0) != 0)
        return;
}
