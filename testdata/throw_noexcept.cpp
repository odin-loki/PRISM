void throws_noexcept_bad(void) noexcept {
    throw 1;
}

void throws_noexcept_ok(void) {
    throw 1;
}

void noexcept_ok(void) noexcept {
    return;
}
