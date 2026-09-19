struct ThrowNewE {};

void throw_new_bad(void) {
    throw new ThrowNewE();
}

void throw_new_ok(void) {
    throw ThrowNewE();
}
