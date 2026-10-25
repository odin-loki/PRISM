int clock_settime(int clk, const void *ts);

void clock_set_bad(void) {
    clock_settime(0,0);
}

void clock_set_ok(void) {
    if (clock_settime(0,0)!=-1)
        return;
}
