struct group { int gr_gid; };
struct group *getgrnam(const char *name);
struct group *getgrgid(unsigned gid);
struct spwd *getspnam(const char *name);

void getgrnam_bad(void) {
    struct group *g=getgrnam("x");
    (void)g->gr_gid;
}

void getgrnam_ok(void) {
    struct group *g=getgrnam("x");
    if (!g)
        return;
    (void)g->gr_gid;
}
