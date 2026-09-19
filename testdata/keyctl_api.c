int keyctl(int cmd, unsigned long a, unsigned long b, unsigned long c,
           unsigned long d);

void keyctl_bad(void) {
    keyctl(0,0,0,0,0);
}

void keyctl_ok(void) {
    if (keyctl(0,0,0,0,0)!=0)
        return;
}
