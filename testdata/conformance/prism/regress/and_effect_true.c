// PRISM conformance task regress/and_effect_true.c: expected true (no-div0)
// regression: && evaluates its right operand only when the left is true
int and_effect_true(int a) {
    int x = 1;
    if (a > 0 && x++ > 0) { a = 0; }
    return 10 / x + a;
}
