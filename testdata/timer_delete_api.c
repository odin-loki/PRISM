int timer_delete(void);

void tdel_bad(void) {
    timer_delete();
}

void tdel_ok(void) {
    if (timer_delete()!=-1)
        return;
}
