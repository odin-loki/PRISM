int pkey_alloc(unsigned flags, unsigned access_rights);
int pkey_mprotect(void *addr, unsigned long len, int prot, int pkey);

void pkey_bad(void) {
    int k=pkey_alloc(0,0);
    if (pkey_mprotect(0,0,0,k)!=-1)
        return;
}

void pkey_ok(void) {
    int k=pkey_alloc(0,0);
    if (k<0)
        return;
    if (pkey_mprotect(0,0,0,k)!=-1)
        return;
}
