int unveil(const char *path, const char *permissions);

void unveil_bad(void) {
    unveil("/", "r");
}

void unveil_ok(void) {
    if (unveil("/", "r") != 0)
        return;
}
