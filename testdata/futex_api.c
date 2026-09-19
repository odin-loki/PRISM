int futex(int *uaddr, int futex_op, int val, void *timeout, int *uaddr2,
          int val3);

void futex_bad(void) {
    futex(0,0,0,0,0,0);
}

void futex_ok(void) {
    if (futex(0,0,0,0,0,0)!=0)
        return;
}
