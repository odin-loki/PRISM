void *dlopen(const char *path, int flags);
int take_handle(void *h);

int dlopen_null_bad(void) {
    void *h = dlopen("x", 0);
    return take_handle(h);
}

int dlopen_null_ok(void) {
    void *h = dlopen("x", 0);
    if (h == 0)
        return 0;
    return take_handle(h);
}
