// PRISM conformance task regress/enum_expr_false.c: expected false (no-div0)
// regression: enums: an enumerator after a computed value is not guessed
enum { KA_F = 1 << 3, KB_F };
int enum_expr_false(int x) {
    return 100 / (x - KB_F);
}
