int pkey_free(int pkey);

void pkeyf_bad(void) {
    pkey_free(0);
}

void pkeyf_ok(void) {
    if (pkey_free(0)!=-1)
        return;
}
