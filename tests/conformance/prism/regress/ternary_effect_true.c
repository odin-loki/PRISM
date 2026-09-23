// PRISM conformance task regress/ternary_effect_true.c: expected true (no-div0)
// regression: ?: runs only the taken arm (its side effects too)
int ternary_effect_true(int a) {
    int x = 1;
    int y = a ? x++ : 0;
    return 10 / x + y;
}
