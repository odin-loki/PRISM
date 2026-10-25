int label_addr_bad(int n) {
    void *p = &&lab;
lab:
    (void)p;
    return n;
}
