// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <stop_token> so the C++ model-header conformance case sees this
// include. request_stop is true once and false the second time.
// stop_requested stays true. A default token is not stop_requested.
// There is no false twin: this header does not plant a compile error.
#include <stop_token>
#include <cassert>

int stop_token_request_true(int) {
    std::stop_source src;
    assert(src.stop_possible());
    assert(!src.stop_requested());
    std::stop_token tok = src.get_token();
    assert(!tok.stop_requested());
    assert(src.request_stop());
    assert(!src.request_stop());
    assert(src.stop_requested());
    assert(tok.stop_requested());
    std::stop_token def;
    assert(!def.stop_requested());
    return 0;
}
