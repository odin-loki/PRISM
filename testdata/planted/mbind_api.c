int mbind(void *addr, unsigned long len, int mode, const unsigned long *nodemask, unsigned long maxnode, unsigned flags);

void mbind_bad(void) {
    mbind(0,0,0,0,0,0);
}

void mbind_ok(void) {
    if (mbind(0,0,0,0,0,0)!=-1)
        return;
}
