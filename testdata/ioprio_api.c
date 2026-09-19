int ioprio_set(int which, int who, int ioprio);

void ioprio_bad(void) {
    ioprio_set(0, 0, 0);
}

void ioprio_ok(void) {
    if (ioprio_set(0, 0, 0)!=-1)
        return;
}
