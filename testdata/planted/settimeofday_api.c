int settimeofday(const void *tv, const void *tz);

void stod_bad(void) {
    settimeofday(0,0);
}

void stod_ok(void) {
    if (settimeofday(0,0)!=-1)
        return;
}
