char *strncpy(char *d, const char *s, unsigned n);

void strncpy_nul_bad(const char *s) {
    char b[4];
    strncpy(b, s, 4);
}

void strncpy_nul_ok(const char *s) {
    char b[4];
    strncpy(b, s, 4);
    b[3] = 0;
}
