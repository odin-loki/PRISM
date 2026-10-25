int setfib(int fib);

void setfib_bad(void) {
    setfib(0);
}

void setfib_ok(void) {
    if (setfib(0) != 0)
        return;
}
