// PRISM conformance task cxx/cxx_lambda_true.cpp: expected true (no-overflow)
int cxx_lambda_true(int a) {
    auto sq = [](int x) { return x * x; };
    if (a < 0 || a > 1000) return 0;
    return sq(a);
}
