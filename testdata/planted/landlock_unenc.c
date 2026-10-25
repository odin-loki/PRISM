void landlock_unenc_bad(void) {
    landlock_create_ruleset();
}

int landlock_unenc_ok(int n) {
    return n;
}
