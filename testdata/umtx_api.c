int _umtx_op(void *obj, int op, unsigned long val, void *uaddr, void *uaddr2);

void umtx_bad(void) {
    _umtx_op(0, 0, 0, 0, 0);
}

void umtx_ok(void) {
    if (_umtx_op(0, 0, 0, 0, 0) != 0)
        return;
}
