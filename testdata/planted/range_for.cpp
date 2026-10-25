int range_for_bad(int n) {
    int xs[2];
    xs[0] = 1;
    xs[1] = 2;
    int s = 0;
    for (int x : xs)
        s += x;
    return s + n;
}

int range_for_ok(int n) {
    int i;
    for (i = 0; i < 2; i++) {
    }
    return n;
}
