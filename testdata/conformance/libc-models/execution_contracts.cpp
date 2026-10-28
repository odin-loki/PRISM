// Names <execution> so the C++ model-header conformance case sees this
// include. The scored task is
// testdata/conformance/prism/cxx/cxx_execution_true.cpp.
// seq is the sequenced policy tag. There is no false twin: this header
// does not plant a defect. This header is not a parallelism proof.
#include <execution>

int execution_seq_true(int n) {
    std::execution::sequenced_policy policy = std::execution::seq;
    int buf[1] = {n};
    if (sizeof(policy) == sizeof(std::execution::sequenced_policy)) return buf[0];
    return buf[0];
}
