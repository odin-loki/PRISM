int mac_set_proc(const void *label);

void mac_bad(void) {
    mac_set_proc(0);
}

void mac_ok(void) {
    if (mac_set_proc(0) != 0)
        return;
}
