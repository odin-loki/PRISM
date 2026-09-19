int cap_sandboxed(void);

void capsand_bad(void) {
    cap_sandboxed();
}

void capsand_ok(void) {
    if (cap_sandboxed() != 0)
        return;
}
