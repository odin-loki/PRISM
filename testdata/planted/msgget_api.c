int msgget(int key, int msgflg);

void msgget_bad(void) {
    msgget(0,0);
}

void msgget_ok(void) {
    if (msgget(0,0)<0)
        return;
}
