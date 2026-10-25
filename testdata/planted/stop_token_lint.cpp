void stop_token_bad(void) {
    std::stop_token st;
    while (true) {
        (void)st;
    }
}

void stop_token_ok(void) {
    std::stop_token st;
    while (!st.stop_requested()) {
        (void)st;
    }
}
