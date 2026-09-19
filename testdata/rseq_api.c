int rseq(void *rseq, unsigned rseq_len, int flags, unsigned sig);

void rseq_bad(void) {
    rseq(0, 0, 0, 0);
}

void rseq_ok(void) {
    if (rseq(0, 0, 0, 0)!=-1)
        return;
}
