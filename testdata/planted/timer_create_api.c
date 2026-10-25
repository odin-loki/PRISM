int timer_create(int clock, void *sev, void *timerid);

void timer_create_bad(void) {
    timer_create(0, 0, 0);
}

void timer_create_ok(void) {
    if (timer_create(0, 0, 0)!=-1)
        return;
}
