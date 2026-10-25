int getcpu(unsigned *cpu, unsigned *node, void *tcache);

void getcpu_bad(void) {
    getcpu(0, 0, 0);
}

void getcpu_ok(void) {
    if (getcpu(0, 0, 0)!=-1)
        return;
}
