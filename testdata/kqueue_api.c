int kqueue(void);

void kqueue_bad(void) {
    kqueue();
}

void kqueue_ok(void) {
    if (kqueue() < 0)
        return;
}
