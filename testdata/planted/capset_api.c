int capset(void *hdr, const void *data);
int capget(void *hdr, void *data);

void capset_bad(void) {
    capset(0,0);
}

void capset_ok(void) {
    if (capset(0,0)!=0)
        return;
}
