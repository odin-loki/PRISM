// PRISM conformance task regress/and_effect_false.c: expected false (no-div0)
// regression: && evaluates its right operand only when the left is true
int and_effect_false(int a) {
    int x = 1;
    if (a > 0 && x++ > 0) { a = 0; }
    return 10 / (x - 1) + a;
}
