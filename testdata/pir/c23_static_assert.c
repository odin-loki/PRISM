// PIR task: C23 static_assert in a unit lowered as C17 (an undeclared call):
// its operand is checked, a constant true one proves.
int sa_bad(int n) {
    static_assert(sizeof(int) == 8);
    return n;
}
int sa_ok(int n) {
    static_assert(sizeof(int) == 4);
    return n;
}
