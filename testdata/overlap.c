void overlap_bad(char *p, int n) {
    memcpy(p, p, n);
}
void overlap_ok(char *p, int n) {
    memmove(p, p, n);
}
void overlap_ok2(char *dst, char *src, int n) {
    memcpy(dst, src, n);
}
