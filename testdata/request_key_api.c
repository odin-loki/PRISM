int request_key(void);

void reqkey_bad(void) {
    request_key();
}

void reqkey_ok(void) {
    if (request_key()!=-1)
        return;
}
