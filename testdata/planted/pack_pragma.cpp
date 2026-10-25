#pragma pack(1)
struct PackS {
    char a;
    int b;
};

void pack_bad(void) {
    struct PackS s;
    int *p = &s.b;
    *p = 1;
}

void pack_ok(void) {
    struct PackS s;
    int v;
    v = s.b;
    (void)v;
}
