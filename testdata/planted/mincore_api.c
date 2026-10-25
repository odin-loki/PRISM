int mincore(void *addr, unsigned long length, unsigned char *vec);

void mincore_bad(void) {
    mincore(0, 0, 0);
}

void mincore_ok(void) {
    if (mincore(0, 0, 0)!=-1)
        return;
}
