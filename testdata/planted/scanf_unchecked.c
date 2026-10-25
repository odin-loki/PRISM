int scanf(const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);

void scanf_bad(int *p) {
    scanf("%d", p);
}

void scanf_ok(int *p) {
    if (scanf("%d", p) != 1)
        return;
}

void sscanf_ok(const char *s, int *p) {
    int n = sscanf(s, "%d", p);
    (void)n;
}
