unsigned long strlcpy(char *dst, const char *src, unsigned long sz);
unsigned long strlcat(char *dst, const char *src, unsigned long sz);

void strlcpy_bad(void) {
    strlcpy(0,0,0);
}

void strlcpy_ok(void) {
    if (strlcpy(0,0,0)>=8)
        return;
}
