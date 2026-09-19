int setfsuid(int fsuid);

void setfsuid_bad(void) {
    setfsuid(0);
}

void setfsuid_ok(void) {
    if (setfsuid(0)!=-1)
        return;
}
