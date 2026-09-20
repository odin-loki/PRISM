int stmtexpr_unenc_bad(int n) {
    return ({ int t; t = n; t; });
}

int stmtexpr_unenc_ok(int n) {
    return n;
}
