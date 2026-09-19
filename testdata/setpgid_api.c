int setpgid(int pid, int pgid);

void setpgid_bad(void) {
    setpgid(0,0);
}

void setpgid_ok(void) {
    if (setpgid(0,0)!=-1)
        return;
}
