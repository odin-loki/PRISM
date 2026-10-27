// PRISM conformance task cxx/cxx_stop_token_true.cpp: expected true (memsafety)
// request_stop is true once and false the second time. stop_requested stays
// true. A default token is not stop_requested. The source outlives the token.
#include <stop_token>
int cxx_stop_token_true(int) {
    std::stop_source src;
    std::stop_token tok = src.get_token();
    std::stop_token def;
    if (def.stop_requested()) return 0;
    if (!src.stop_possible()) return 0;
    if (!src.request_stop()) return 0;
    if (src.request_stop()) return 0;
    if (!src.stop_requested() || !tok.stop_requested()) return 0;
    return 1;
}
