int getgroups(int size, int *list);

void getgroups_bad(void) {
    getgroups(0,0);
}

void getgroups_ok(void) {
    if (getgroups(0,0)!=-1)
        return;
}
