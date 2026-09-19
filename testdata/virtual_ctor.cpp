struct Widget {
    virtual void start();
};

void Widget_ctor_bad(void) {
    start();
}

void Widget_ctor_ok(void) {
    int x = 0;
    (void)x;
}

struct Base {
    virtual void virt();
    Base() { virt(); }
};
