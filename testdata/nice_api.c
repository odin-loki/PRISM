int nice(int inc);

void nice_bad(void) {
    nice(0);
}

void nice_ok(void) {
    if (nice(0)!=-1)
        return;
}
