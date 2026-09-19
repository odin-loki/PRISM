[[nodiscard]]
int must_use(void) {
    return 1;
}

void nodiscard_bad(void) {
    must_use();
}

int nodiscard_ok(void) {
    int r = must_use();
    return r;
}
