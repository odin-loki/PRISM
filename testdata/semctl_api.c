int semctl(int semid, int semnum, int cmd);

void semctl_bad(void) {
    semctl(0, 0, 0);
}

void semctl_ok(void) {
    if (semctl(0, 0, 0)!=-1)
        return;
}
