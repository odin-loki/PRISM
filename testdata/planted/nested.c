int nested_ovf(int n) {
    int s;
    int i;
    int j;
    s = 0;
    i = 0;
    while (i < n) {
        j = 0;
        while (j < 2) {
            s = s + 1073741824;
            j = j + 1;
        }
        i = i + 1;
    }
    return s;
}

int nested_ok(void) {
    int s;
    int i;
    int j;
    s = 0;
    i = 0;
    while (i < 2) {
        j = 0;
        while (j < 2) {
            s = s + 1;
            j = j + 1;
        }
        i = i + 1;
    }
    return s;
}
