void mismatch_free_bad(void) {
    int *p = (int *)malloc(sizeof(int));
    delete p;
}

void mismatch_free_ok(void) {
    int *p = (int *)malloc(sizeof(int));
    free(p);
}
