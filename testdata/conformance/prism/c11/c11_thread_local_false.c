// PRISM conformance task c11/c11_thread_local_false.c: expected false (no-div0)
static _Thread_local int tl = 1;
int c11_thread_local_false(int x) {
    tl = x & 7;
    return 100 / tl;
}
