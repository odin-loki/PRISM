void bcopy(const void *src, void *dst, unsigned n);
void memmove(void *dst, const void *src, unsigned n);

void bcopy_bad(char *p, int n) {
    bcopy(p, p, n);
}

void bcopy_ok(char *src, char *dst, int n) {
    bcopy(src, dst, n);
}

void bcopy_memmove_ok(char *p, int n) {
    memmove(p, p, n);
}
