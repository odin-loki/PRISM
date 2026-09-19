int semget(int key, int nsems, int semflg);

void semget_bad(void) {
    semget(0,0,0);
}

void semget_ok(void) {
    if (semget(0,0,0)<0)
        return;
}
