void jwork(void) {
}

void jthread_bad(void) {
    std::jthread t(jwork);
}

void jthread_ok(void) {
    std::jthread t(jwork);
    t.request_stop();
}
