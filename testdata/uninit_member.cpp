struct UninitMemBad {
    int x;
    UninitMemBad() {}
};

int uninit_mem_bad(void) {
    UninitMemBad c;
    return c.x;
}

struct UninitMemOk {
    int x;
    UninitMemOk() : x(0) {}
};

int uninit_mem_ok(void) {
    UninitMemOk c;
    return c.x;
}
