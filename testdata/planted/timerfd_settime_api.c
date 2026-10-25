int timerfd_settime(int fd, int flags, const void *new_value, void *old_value);

void tfds_bad(void) {
    timerfd_settime(0,0,0,0);
}

void tfds_ok(void) {
    if (timerfd_settime(0,0,0,0)!=-1)
        return;
}
