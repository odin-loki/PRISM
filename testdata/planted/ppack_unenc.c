#pragma pack(1)
int ppack_unenc_bad(int n) {
#pragma pack(1)
    struct { char a; int b; } s;
    (void)s;
    return n;
}
