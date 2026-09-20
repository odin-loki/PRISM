int dcast_unenc_bad(int n) {
    CxxDer *q = dynamic_cast<CxxDer*>(p);
    (void)q;
    return n;
}

int dcast_unenc_ok(int n) {
    return n;
}
