int ioperm(unsigned long from, unsigned long num, int turn_on);

void ioperm_bad(void) {
    ioperm(0, 0, 0);
}

void ioperm_ok(void) {
    if (ioperm(0, 0, 0)!=-1)
        return;
}
