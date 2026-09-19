int getaddrinfo(const char *node, const char *service, const void *hints, void **res);

void getaddrinfo_bad(void) {
    void *r;
    getaddrinfo("h", "80", 0, &r);
    (void)r;
}

void getaddrinfo_ok(void) {
    void *r;
    if (getaddrinfo("h", "80", 0, &r) != 0)
        return;
}
