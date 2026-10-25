int mq_open(const char *name, int oflag);

void mq_open_bad(void) {
    int q=mq_open("x",0);
    (void)q;
}

void mq_open_ok(void) {
    int q=mq_open("x",0);
    if (q<0)
        return;
    (void)q;
}
