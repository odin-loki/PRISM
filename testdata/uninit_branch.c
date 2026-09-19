int uninit_br_bad(void) {
    int x;
    if (x)
        return 1;
    return 0;
}

int uninit_br_ok(void) {
    int x = 0;
    if (x)
        return 1;
    return 0;
}

int uninit_br_ok2(void) {
    int x;
    x = 1;
    if (x)
        return 1;
    return 0;
}
