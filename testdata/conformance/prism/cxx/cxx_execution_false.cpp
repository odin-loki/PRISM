// PRISM conformance task cxx/cxx_execution_false.cpp: expected false (no-oob)
// the seq tag does not bound the index: buf[1] on a one-int buffer
#include <execution>
int cxx_execution_false(int n) {
    std::execution::sequenced_policy policy = std::execution::seq;
    (void)policy;
    int buf[1] = {n};
    if (n < 0 || n > 1) return 0;
    return buf[n];
}
