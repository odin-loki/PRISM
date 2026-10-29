// PRISM conformance task cxx/cxx_stop_token_false.cpp: expected false (memsafety)
// after request_stop the index is 2, one past a two-int buffer
#include <stop_token>
int cxx_stop_token_false(int n) {
    std::stop_source s;
    s.request_stop();
    int buf[2] = {0, 1};
    const int idx = s.stop_requested() ? 2 : 0;
    if (n != 1) return 0;
    return buf[idx];
}
