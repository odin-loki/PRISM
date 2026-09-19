int getrusage(int who, void *usage);

void getrusage_bad(void) {
    getrusage(0,0);
}

void getrusage_ok(void) {
    if (getrusage(0,0)!=0)
        return;
}
