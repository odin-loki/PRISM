/* PIR memory tasks: arrays on the stack and in static storage (docs/PIR.md
 * "Memory model"). *_ok: no undefined behaviour for any input; *_bad: a
 * violation exists (the class in tests/test_pir.py MEM_CLASSES). */
int table[5] = {1, 2, 3, 4, 5};

int arr_read_ok(int i) {
    int a[4] = {1, 2, 3, 4};
    if (i < 0 || i >= 4) return 0;
    return a[i];
}

int arr_read_bad(int i) {
    int a[4] = {1, 2, 3, 4};
    if (i < 0 || i > 4) return 0; /* i == 4 reads past the end */
    return a[i];
}

int arr_write_ok(int i) {
    int a[4] = {0, 0, 0, 0};
    if (i >= 0 && i < 4) a[i] = 7;
    return a[0];
}

int arr_write_bad(int i) {
    int a[4] = {0, 0, 0, 0};
    if (i >= -1 && i < 4) a[i] = 7; /* i == -1 writes before the start */
    return a[1];
}

int arr_2d_ok(int i, int j) {
    int m[3][2] = {{1, 2}, {3, 4}, {5, 6}};
    if (i < 0 || i > 2 || j < 0 || j > 1) return 0;
    return m[i][j];
}

int global_ok(int i) {
    if (i < 0 || i > 4) return 0;
    return table[i] > 3;
}

int global_bad(int i) {
    if (i < 0 || i > 5) return 0;
    return table[i] > 3; /* table has 5 elements */
}

int struct_ok(int v) {
    struct {
        int a;
        char b;
        long c;
    } s;
    s.a = v;
    s.b = 1;
    s.c = 2;
    return s.b + (int)s.c + (s.a > 0);
}

int uninit_mem_bad(int i) {
    int a[4];
    a[0] = 1;
    return a[i & 3]; /* a[1..3] never written */
}

int uninit_mem_ok(int i) {
    int a[4];
    for (int k = 0; k < 4; k++) a[k] = k;
    return a[i & 3];
}
