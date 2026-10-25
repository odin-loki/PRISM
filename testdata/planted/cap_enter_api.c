int cap_enter(void);

void capenter_bad(void) {
    cap_enter();
}

void capenter_ok(void) {
    if (cap_enter() != 0)
        return;
}
