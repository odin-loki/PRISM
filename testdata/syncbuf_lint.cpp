void syncbuf_bad(void) {
    std::syncbuf buf(std::cout.rdbuf());
}

void syncbuf_ok(void) {
    std::syncbuf buf(std::cout.rdbuf());
    buf.emit();
}
