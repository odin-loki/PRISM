void enable_shared_bad(void) {
    shared_from_this();
}

void enable_shared_ok(void) {
    enable_shared_from_this<Node> n;
    shared_from_this();
}
