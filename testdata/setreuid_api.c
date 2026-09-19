int setreuid(int ruid, int euid);

void setreuid_bad(void) {
    setreuid(0,0);
}

void setreuid_ok(void) {
    if (setreuid(0,0)!=-1)
        return;
}
