int msgsnd(void);

void msgsnd_bad(void) {
    msgsnd();
}

void msgsnd_ok(void) {
    if (msgsnd()!=-1)
        return;
}
