// PRISM conformance task regress/enum_expr_true.c: expected true (no-div0)
// regression: enums: an enumerator after a computed value is not guessed
enum { KA_T = 1 << 3, KB_T };
int enum_expr_true(int x) {
    if (x < 0 || x == KB_T) return 0;
    return 100 / (x - KB_T);
}
