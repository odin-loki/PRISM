int getdomainname(char *name, int namelen);

void gdname_bad(void) {
    getdomainname(0, 0);
}

void gdname_ok(void) {
    if (getdomainname(0, 0) != 0)
        return;
}
