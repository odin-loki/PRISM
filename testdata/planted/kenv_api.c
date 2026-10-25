int kenv(int action, const char *name, char *value, int len);

void kenv_bad(void) {
    kenv(0, "x", 0, 0);
}

void kenv_ok(void) {
    if (kenv(0, "x", 0, 0) != 0)
        return;
}
