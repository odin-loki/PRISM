/* BMC C subset: do-while, sizeof, ternary. */

int do_once(int x) {
    int i;
    i = 0;
    do {
        i = i + 1;
    } while (i < 0);
    return i;
}

int do_overflow(int n) {
    int s;
    s = n;
    do {
        s = s + n;
    } while (0);
    return s;
}

int sz_int(void) {
    return sizeof(int);
}

int sz_arr(void) {
    int a[4];
    return sizeof(a);
}

int pick(int x) {
    return x > 0 ? 1 : 0;
}

int abs_ter(int x) {
    return x < 0 ? -x : x;
}
