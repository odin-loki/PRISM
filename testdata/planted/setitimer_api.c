int setitimer(int which, const void *new_value, void *old_value);

void sitimer_bad(void) {
    setitimer(0,0,0);
}

void sitimer_ok(void) {
    if (setitimer(0,0,0)!=-1)
        return;
}
