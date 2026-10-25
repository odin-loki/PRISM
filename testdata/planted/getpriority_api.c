int getpriority(int which, int who);
int setpriority(int which, int who, int prio);

void getpriority_bad(void) {
    getpriority(0,0);
}

void getpriority_ok(void) {
    if (getpriority(0,0) != -1)
        return;
}
