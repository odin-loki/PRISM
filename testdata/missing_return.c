int missing_return_bad(void) {
    int x;
    x = 1;
}

int missing_return_ok(void) {
    int x;
    x = 1;
    return x;
}

int missing_return_oneline_ok(void) { int x = 1; return x; }

int missing_return_oneline_bad(void) { int x = 1; x = 2; }
