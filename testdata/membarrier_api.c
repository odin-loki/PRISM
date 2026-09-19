int membarrier(int cmd, unsigned flags);

void membarrier_bad(void) {
    membarrier(0,0);
}

void membarrier_ok(void) {
    if (membarrier(0,0)!=0)
        return;
}
