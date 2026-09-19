int nfssvc(int flags, void *argstructp);

void nfssvc_bad(void) {
    nfssvc(0, 0);
}

void nfssvc_ok(void) {
    if (nfssvc(0, 0) != 0)
        return;
}
