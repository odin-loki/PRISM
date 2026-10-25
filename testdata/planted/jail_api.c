int jail(void *jp);
int jail_attach(int jid);
int jail_get(void *iov, unsigned niov, int flags);
int jail_set(void *iov, unsigned niov, int flags);
int jail_remove(int jid);

void jail_bad(void) {
    jail(0);
}

void jail_ok(void) {
    if (jail(0) != 0)
        return;
}
