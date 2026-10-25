#define MAX 100

void taut_bound_bad(int idx) {
    if (idx >= 0 || idx < MAX)
        return;
}

void taut_bound_ok(int i, int n) {
    if (i >= 0 && i < n)
        return;
}
