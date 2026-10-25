void thrd_unenc_bad(void) {
    thrd_create();
}

void thrd_sleep_unenc_bad(void) { thrd_sleep(); }
void thrd_yield_unenc_bad(void) { thrd_yield(); }
void thrd_current_unenc_bad(void) { thrd_current(); }
void thrd_equal_unenc_bad(void) { thrd_equal(); }
void thrd_exit_unenc_bad(void) { thrd_exit(); }
void thrd_join_unenc_bad(void) { thrd_join(); }
void thrd_detach_unenc_bad(void) { thrd_detach(); }

int thrd_unenc_ok(int n) {
    return n;
}
