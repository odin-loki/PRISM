int modfind(const char *modname);

void modfind_bad(void) {
    modfind("x");
}

void modfind_ok(void) {
    if (modfind("x") < 0)
        return;
}
