int getpeereid(int s, int *euid, int *egid);

void getpeereid_bad(void) {
    getpeereid(0, 0, 0);
}

void getpeereid_ok(void) {
    if (getpeereid(0, 0, 0) != 0)
        return;
}
