void landlock_restrict_self_unenc_bad(void) {
    landlock_restrict_self();
}

int landlock_restrict_self_unenc_ok(int n) {
    return n;
}
