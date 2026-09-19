int trunc_bad(int n) {
    char c = n;
    short s = n;
    char d = (char)n;
    return c + s + d;
}

int trunc_ok(void) {
    char c = 1;
    char d = 'A';
    int x = 42;
    return c + d + x;
}

void trunc_assign_bad(int n) {
    char c;
    c = n;
}
