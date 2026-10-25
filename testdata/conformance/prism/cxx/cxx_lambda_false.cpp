// PRISM conformance task cxx/cxx_lambda_false.cpp: expected false (no-overflow)
int cxx_lambda_false(int a) {
    auto sq = [](int x) { return x * x; };
    return sq(a);
}
