int inet_pton(int af, const char *src, void *dst);

void inet_pton_bad(void) {
    inet_pton(0,0,0);
}

void inet_pton_ok(void) {
    if (inet_pton(0,0,0)!=1)
        return;
}
