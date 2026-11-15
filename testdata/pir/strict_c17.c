// PIR task: the strict C17 default stays. `typeof` and `asm` are ordinary
// identifiers in ISO C17 and keywords in GNU C, so this unit compiles only
// as -std=c17: the front end must not switch to gnu17 for a unit that
// already compiles (docs/PIR.md "GNU C units").
int typeof_name_ok(int n) {
    int typeof = n | 1;
    int asm = 2;
    return (typeof & 1) + 100 / asm;
}

int typeof_name_bad(int n) {
    int typeof = n;
    int asm = 0;
    return 100 / (typeof + asm);
}
