int semop(void);

void semop_bad(void) {
    semop();
}

void semop_ok(void) {
    if (semop()!=-1)
        return;
}
