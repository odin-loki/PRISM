struct passwd { char *pw_name; };
struct passwd *getpwuid(unsigned uid);

void getpwuid_bad(void) {
    struct passwd *p=getpwuid(0);
    (void)p->pw_name;
}

void getpwuid_ok(void) {
    struct passwd *p=getpwuid(0);
    if (!p)
        return;
    (void)p->pw_name;
}
