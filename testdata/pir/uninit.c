/* PIR tasks: read of an uninitialised local. */
int uninit_bad(int c) {
    int x;
    if (c)
        x = 1;
    return x;
}
int uninit_ok(int c) {
    int x;
    if (c)
        x = 1;
    else
        x = 2;
    return x;
}
