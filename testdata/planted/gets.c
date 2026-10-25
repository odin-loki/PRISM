char *gets(char *s);
char *fgets(char *s, int n, void *fp);

void gets_bad(char *p) {
    gets(p);
}

void gets_ok(char *p) {
    fgets(p, 16, 0);
}
