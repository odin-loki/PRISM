int add_key(const char *type, const char *description, const void *payload,
            unsigned long plen, int keyring);

void addkey_bad(void) {
    add_key(0, 0, 0, 0, 0);
}

void addkey_ok(void) {
    if (add_key(0, 0, 0, 0, 0)!=-1)
        return;
}
