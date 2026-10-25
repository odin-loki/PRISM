int shmget(int key, unsigned size, int shmflg);

void shmget_bad(void) {
    shmget(0,0,0);
}

void shmget_ok(void) {
    if (shmget(0,0,0)<0)
        return;
}
