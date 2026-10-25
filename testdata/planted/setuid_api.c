int setuid(int uid);
int seteuid(int uid);
int setgid(int gid);

void setuid_bad(void) {
    setuid(0);
}

void setuid_ok(void) {
    if (setuid(0) != 0)
        return;
}
