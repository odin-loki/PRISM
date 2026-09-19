int procctl(int idtype, int id, int cmd, void *data);

void procctl_bad(void) {
    procctl(0, 0, 0, 0);
}

void procctl_ok(void) {
    if (procctl(0, 0, 0, 0) != 0)
        return;
}
