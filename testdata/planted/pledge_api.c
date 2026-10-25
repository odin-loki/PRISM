int pledge(const char *promises, const char *execpromises);

void pledge_bad(void) {
    pledge("stdio", 0);
}

void pledge_ok(void) {
    if (pledge("stdio", 0) != 0)
        return;
}
