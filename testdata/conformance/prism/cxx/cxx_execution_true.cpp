// PRISM conformance task cxx/cxx_execution_true.cpp: expected true (no-oob).
// seq is the sequenced policy tag. The tag does not choose an index.
// Both branches read buf[0]. This header is not a parallelism proof.
#include <execution>
int cxx_execution_true(int n) {
    std::execution::sequenced_policy policy = std::execution::seq;
    int buf[1] = {n};
    if (sizeof(policy) == sizeof(std::execution::sequenced_policy)) return buf[0];
    return buf[0];
}
