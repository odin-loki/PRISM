int kcmp(int pid1, int pid2, int type, unsigned long idx1, unsigned long idx2);

void kcmp_bad(void) {
    kcmp(0,0,0,0,0);
}

void kcmp_ok(void) {
    if (kcmp(0,0,0,0,0)!=0)
        return;
}
