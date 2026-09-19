int getresuid(int *ruid, int *euid, int *suid);
int getresgid(int *rgid, int *egid, int *sgid);

void getresuid_bad(void) {
    getresuid(0, 0, 0);
}

void getresuid_ok(void) {
    if (getresuid(0, 0, 0) != 0)
        return;
}
