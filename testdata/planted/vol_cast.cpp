int vol_cast_bad(void) {
    volatile int vol = 0;
    *(int*)&vol = 1;
    return vol;
}

int vol_cast_ok(void) {
    volatile int vol = 0;
    vol = 1;
    return vol;
}
