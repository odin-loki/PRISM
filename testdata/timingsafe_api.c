int timingsafe_bcmp(const void *b1, const void *b2, unsigned len);
int timingsafe_memcmp(const void *b1, const void *b2, unsigned len);

void tsafe_bad(void) {
    timingsafe_bcmp(0, 0, 0);
}

void tsafe_ok(void) {
    if (timingsafe_bcmp(0, 0, 0) != 0)
        return;
}
