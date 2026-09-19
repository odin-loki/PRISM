int clock_gettime(int clk, void *ts);

void clock_bad(void) {
    clock_gettime(0,0);
}

void clock_ok(void) {
    if (clock_gettime(0,0)!=0)
        return;
}
