// PRISM conformance task c11/c11_thread_local_true.c: expected true (no-div0)
static _Thread_local int tl = 1;
int c11_thread_local_true(int x) {
    tl = (x & 7) + 1;
    return 100 / tl;
}
