int mq_unlink(void);

void mqun_bad(void) {
    mq_unlink();
}

void mqun_ok(void) {
    if (mq_unlink()!=-1)
        return;
}
