int getpagesizes(int nelem, unsigned long *pagesize);

void gps_bad(void) {
    getpagesizes(0, 0);
}

void gps_ok(void) {
    if (getpagesizes(0, 0) < 0)
        return;
}
