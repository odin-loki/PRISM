void atom_qual_bad(void) {
    _Atomic int x;
    x = 1;
}

int vol_ok(int x) {
    return x;
}
