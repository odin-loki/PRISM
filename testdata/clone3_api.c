int clone3(void *args, unsigned long size);

void clone3_bad(void) {
    clone3(0,0);
}

void clone3_ok(void) {
    if (clone3(0,0)<0)
        return;
}
