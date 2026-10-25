int unshare(int flags);

void unshare_bad(void) {
    unshare(0);
}

void unshare_ok(void) {
    if (unshare(0)!=0)
        return;
}
