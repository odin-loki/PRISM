int generic_sel(int x) {
    return _Generic(x, int: 1, long: 2);
}

int stmt_expr_id(int x) {
    return ({ int t; t = x; t; });
}

int off_field(int x) {
    return offsetof(struct S, f);
}
