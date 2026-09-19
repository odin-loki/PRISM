int klogctl(int type, char *bufp, int len);

void klogctl_bad(void) {
    klogctl(0, 0, 0);
}

void klogctl_ok(void) {
    if (klogctl(0, 0, 0)!=-1)
        return;
}
