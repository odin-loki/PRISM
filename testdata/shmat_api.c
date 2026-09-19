void *shmat(void);

void shmat_bad(void) {
    shmat();
}

void shmat_ok(void) {
    if (shmat()!=-1)
        return;
}
