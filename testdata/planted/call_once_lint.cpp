void call_once_bad(void) {
    std::call_once(1, []{});
}

void call_once_ok(void) {
    std::once_flag f;
    std::call_once(f, []{});
}
