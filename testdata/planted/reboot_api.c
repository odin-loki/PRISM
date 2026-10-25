int reboot(int cmd);

void reboot_bad(void) {
    reboot(0);
}

void reboot_ok(void) {
    if (reboot(0)!=-1)
        return;
}
